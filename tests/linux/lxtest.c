/*
 * lxtest - Exercise the Linux personality through the raw Linux ABI
 * (docs/compat/linux/testing.md). Freestanding: no libc, no CosmoOS
 * note, so the kernel runs it as a Linux program. Prints
 * "LINUXTEST: PASS" or "LINUXTEST: FAIL <what>" and exits 0 or 1.
 */

#include "lxabi.h"

static int g_failures;

static void put_num(long v)
{
    char buf[24];
    int i = sizeof(buf);
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-(v + 1)) + 1 : (unsigned long)v;
    buf[--i] = '\0';
    do {
        buf[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (neg)
        buf[--i] = '-';
    lx_puts(buf + i);
}

static void check(int cond, const char *what, long value)
{
    if (cond)
        return;
    g_failures++;
    lx_puts("LINUXTEST: FAIL ");
    lx_puts(what);
    lx_puts(" (");
    put_num(value);
    lx_puts(")\n");
}
#define CHECK(c) check((c), #c, 0)
#define CHECKV(c, v) check((c), #c, (long)(v))

static int memeq(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i])
            return 0;
    return 1;
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* The thread pointer: %fs:0 reads the word at the FS base. */
static unsigned long read_fs0(void)
{
    unsigned long v;
    __asm__ volatile("movq %%fs:0, %0" : "=r"(v));
    return v;
}

int main(int argc, char **argv)
{
    CHECKV(argc >= 1 && argv[0][0] != '\0', argc);

    /* --- identity, uname, time --- */
    long pid = sc0(LX_getpid);
    CHECKV(pid > 0, pid);
    CHECKV(sc0(LX_gettid) == pid, 0);
    CHECKV(sc0(LX_getppid) > 0, 0);
    CHECKV(sc0(LX_getuid) == 0 && sc0(LX_geteuid) == 0, 0);
    struct lx_utsname u;
    CHECKV(sc1(LX_uname, &u) == 0, 0);
    CHECK(streq(u.sysname, "Linux") && streq(u.machine, "x86_64"));
    struct lx_timespec ts0, ts1;
    CHECKV(sc2(LX_clock_gettime, LX_CLOCK_MONOTONIC, &ts0) == 0, 0);
    struct lx_timespec nap = { 0, 5000000 };
    CHECKV(sc2(LX_nanosleep, &nap, 0) == 0, 0);
    CHECKV(sc2(LX_clock_gettime, LX_CLOCK_REALTIME, &ts1) == 0, 0);
    long dt = (ts1.tv_sec - ts0.tv_sec) * 1000000000L + (ts1.tv_nsec - ts0.tv_nsec);
    CHECKV(dt >= 5000000 && dt < 500000000, dt);
    CHECKV(sc2(LX_clock_gettime, 99, &ts1) == -22, 0);   /* EINVAL */

    /* --- thread pointer --- */
    static unsigned long tcb[4] = { 0x1234567887654321UL, 0, 0, 0 };
    CHECKV(sc2(LX_arch_prctl, LX_ARCH_SET_FS, tcb) == 0, 0);
    CHECKV(read_fs0() == 0x1234567887654321UL, 0);
    unsigned long fs = 0;
    CHECKV(sc2(LX_arch_prctl, LX_ARCH_GET_FS, &fs) == 0 && fs == (unsigned long)tcb, fs);
    CHECKV(sc2(LX_arch_prctl, LX_ARCH_SET_GS, tcb) == -22, 0);
    long tid = sc1(LX_set_tid_address, &tcb[1]);
    CHECKV(tid == pid, tid);
    /* The pointer survives a sleep (a context switch away and back). */
    nap.tv_nsec = 2000000;
    sc2(LX_nanosleep, &nap, 0);
    CHECKV(read_fs0() == 0x1234567887654321UL, 0);

    /* --- brk and mmap --- */
    long brk0 = sc1(LX_brk, 0);
    CHECKV(brk0 > 0 && (brk0 & 0xfff) == 0, brk0);
    long brk1 = sc1(LX_brk, brk0 + 100000);
    CHECKV(brk1 == brk0 + 100000, brk1);
    volatile unsigned char *heap = (unsigned char *)brk0;
    heap[0] = 1;
    heap[99999] = 2;
    CHECK(heap[0] == 1 && heap[99999] == 2);
    CHECKV(sc1(LX_brk, brk0) == brk0, 0);                     /* shrink back */
    CHECKV(sc1(LX_brk, brk0 - 4096) == brk0, 0);              /* below the start: unchanged */
    long m = sc6(LX_mmap, 0, 8192, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS, -1, 0);
    CHECKV(m > 0 && (m & 0xfff) == 0, m);
    volatile unsigned int *w = (unsigned int *)m;
    CHECK(w[0] == 0 && w[2047] == 0);
    w[0] = 7;
    w[2047] = 9;
    CHECK(w[0] == 7 && w[2047] == 9);
    CHECKV(sc3(LX_mprotect, m, 8192, LX_PROT_READ) == 0, 0);
    CHECK(w[0] == 7);
    CHECKV(sc2(LX_munmap, m, 8192) == 0, 0);
    CHECKV(sc6(LX_mmap, 0, 4096, LX_PROT_READ, LX_MAP_PRIVATE, 3, 0) == -19, 0);   /* file mapping: ENODEV */
    CHECKV(sc6(LX_mmap, 0, 4096, LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS, -1, 0) == -22, 0);
    long fixed = sc6(LX_mmap, 0x30000000000UL, 4096, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS | LX_MAP_FIXED, -1, 0);
    CHECKV(fixed == 0x30000000000L, fixed);
    if (fixed > 0) {
        *(volatile char *)fixed = 'x';
        CHECKV(sc2(LX_munmap, fixed, 4096) == 0, 0);
    }

    /* --- files --- */
    long fd = sc4(LX_openat, LX_AT_FDCWD, "/tmp/lxtest.txt", LX_O_RDWR | LX_O_CREAT | LX_O_TRUNC | LX_O_CLOEXEC, 0644);
    CHECKV(fd >= 3, fd);
    CHECKV(sc3(LX_write, fd, "linux abi\n", 10) == 10, 0);
    struct lx_iovec iov[2] = { { (uint64_t)(uintptr_t) "second ", 7 }, { (uint64_t)(uintptr_t) "line\n", 5 } };
    CHECKV(sc3(LX_writev, fd, iov, 2) == 12, 0);
    struct lx_stat st;
    CHECKV(sc2(LX_fstat, fd, &st) == 0, 0);
    CHECKV((st.st_mode & LX_S_IFMT) == LX_S_IFREG && st.st_size == 22 && st.st_nlink == 1, st.st_mode);
    CHECKV(sizeof(struct lx_stat) == 144, sizeof(struct lx_stat));
    char buf[64];
    CHECKV(sc4(LX_pread64, fd, buf, 4, 6) == 4 && memeq(buf, "abi\n", 4), 0);
    CHECKV(sc3(LX_lseek, fd, 0, 0) == 0, 0);
    char b1[8], b2[16];
    struct lx_iovec riov[2] = { { (uint64_t)(uintptr_t)b1, 6 }, { (uint64_t)(uintptr_t)b2, 16 } };
    CHECKV(sc3(LX_readv, fd, riov, 2) == 22 && memeq(b1, "linux ", 6) && memeq(b2, "abi\nsecond line\n", 16), 0);
    CHECKV(sc3(LX_read, fd, buf, 8) == 0, 0);                  /* EOF */
    CHECKV(sc1(LX_close, fd) == 0, 0);
    CHECKV(sc1(LX_close, fd) == -9, 0);                        /* EBADF */
    CHECKV(sc4(LX_newfstatat, LX_AT_FDCWD, "/tmp/lxtest.txt", &st, 0) == 0 && st.st_size == 22, 0);
    CHECKV(sc2(LX_stat, "/tmp/nope", &st) == -2, 0);           /* ENOENT */
    CHECKV(sc2(LX_stat, "/tmp", &st) == 0 && (st.st_mode & LX_S_IFMT) == LX_S_IFDIR, 0);
    CHECKV(sc2(LX_fstat, 0, &st) == 0 && (st.st_mode & LX_S_IFMT) == LX_S_IFCHR, st.st_mode);
    CHECKV(sc3(LX_ioctl, 1, 0x5401, buf) == -25, 0);           /* TCGETS: ENOTTY */
    CHECKV(sc2(LX_access, "/tmp/lxtest.txt", 0) == 0, 0);
    CHECKV(sc3(LX_mkdirat, LX_AT_FDCWD, "/tmp/lxdir", 0755) == 0, 0);
    CHECKV(sc2(LX_rename, "/tmp/lxtest.txt", "/tmp/lxdir/moved") == 0, 0);
    /* getdents64 on the directory: ".", "..", "moved" */
    long dfd = sc4(LX_openat, LX_AT_FDCWD, "/tmp/lxdir", LX_O_RDONLY | LX_O_DIRECTORY, 0);
    CHECKV(dfd >= 3, dfd);
    static char dents[1024];
    long n = sc3(LX_getdents64, dfd, dents, sizeof(dents));
    CHECKV(n > 0, n);
    int seen = 0;
    for (long off = 0; off < n;) {
        struct lx_dirent64 *d = (struct lx_dirent64 *)(dents + off);
        if (streq(d->d_name, "moved") && d->d_type == LX_DT_REG)
            seen |= 1;
        if (streq(d->d_name, "."))
            seen |= 2;
        if (streq(d->d_name, ".."))
            seen |= 4;
        CHECKV(d->d_reclen >= 24 && (d->d_reclen & 7) == 0, d->d_reclen);
        off += d->d_reclen;
    }
    CHECKV(seen == 7, seen);
    CHECKV(sc3(LX_getdents64, dfd, dents, sizeof(dents)) == 0, 0);
    CHECKV(sc1(LX_close, dfd) == 0, 0);
    CHECKV(sc3(LX_unlinkat, LX_AT_FDCWD, "/tmp/lxdir/moved", 0) == 0, 0);
    CHECKV(sc3(LX_unlinkat, LX_AT_FDCWD, "/tmp/lxdir", LX_AT_REMOVEDIR) == 0, 0);
    CHECKV(sc1(LX_chdir, "/tmp") == 0, 0);
    CHECKV(sc2(LX_getcwd, buf, sizeof(buf)) == 5 && streq(buf, "/tmp"), 0);   /* length includes the NUL */
    CHECKV(sc1(LX_chdir, "/") == 0, 0);
    CHECKV(sc0(LX_umask) == 022, 0);

    /* --- pipes, dup, fcntl --- */
    int32_t p[2];
    CHECKV(sc2(LX_pipe2, p, LX_O_CLOEXEC) == 0, 0);
    CHECKV(sc3(LX_write, p[1], "xyz", 3) == 3, 0);
    CHECKV(sc3(LX_read, p[0], buf, 8) == 3 && memeq(buf, "xyz", 3), 0);
    long d = sc1(LX_dup, p[1]);
    CHECKV(d >= 3 && d != p[1], d);
    CHECKV(sc3(LX_dup3, p[0], 40, 0) == 40, 0);
    CHECKV(sc3(LX_dup3, p[0], p[0], 0) == -22, 0);
    CHECKV(sc3(LX_fcntl, p[1], LX_F_GETFL, 0) == LX_O_WRONLY, 0);
    CHECKV(sc3(LX_fcntl, p[0], LX_F_DUPFD, 50) >= 50, 0);
    CHECKV(sc1(LX_close, p[1]) == 0 && sc1(LX_close, d) == 0, 0);
    CHECKV(sc3(LX_read, 40, buf, 8) == 0, 0);                  /* every writer closed: EOF */
    CHECKV(sc2(LX_fstat, 40, &st) == 0 && (st.st_mode & LX_S_IFMT) == LX_S_IFIFO, 0);
    sc1(LX_close, 40);
    sc1(LX_close, p[0]);

    /* --- signals (stored only), wait, kill --- */
    struct lx_sigaction act = { .handler = 0x400000, .flags = 0x04000000 }, old;
    CHECKV(sc4(LX_rt_sigaction, 2, &act, 0, 8) == 0, 0);
    CHECKV(sc4(LX_rt_sigaction, 2, 0, &old, 8) == 0 && old.handler == 0x400000, 0);
    CHECKV(sc4(LX_rt_sigaction, 9, &act, 0, 8) == -22, 0);    /* SIGKILL */
    uint64_t set = 1ull << 1, oset = 0;
    CHECKV(sc4(LX_rt_sigprocmask, 0, &set, &oset, 8) == 0 && oset == 0, 0);
    CHECKV(sc4(LX_rt_sigprocmask, 0, 0, &oset, 8) == 0 && oset == set, 0);
    int32_t status;
    CHECKV(sc4(LX_wait4, -1, &status, 0, 0) == -10, 0);       /* ECHILD: no children */
    CHECKV(sc2(LX_kill, 999999, 15) == -3, 0);                 /* ESRCH */
    CHECKV(sc2(LX_kill, pid, 0) == 0, 0);                       /* existence probe */
    CHECKV(sc1(LX_execve, "/bin/true") == -38, 0);             /* ENOSYS */
    CHECKV(sc0(LX_fork) == -38, 0);
    CHECKV(sc0(LX_sched_yield) == 0, 0);

    /* --- futex --- */
    static uint32_t word = 5;
    CHECKV(sc6(LX_futex, &word, LX_FUTEX_WAIT | LX_FUTEX_PRIVATE_FLAG, 6, 0, 0, 0) == -11, 0);   /* EAGAIN */
    struct lx_timespec to = { 0, 20000000 };
    long t0 = sc0(LX_time);
    CHECKV(sc6(LX_futex, &word, LX_FUTEX_WAIT, 5, &to, 0, 0) == -110, 0);   /* ETIMEDOUT */
    (void)t0;
    CHECKV(sc6(LX_futex, &word, LX_FUTEX_WAKE, 1, 0, 0, 0) == 0, 0);
    CHECKV(sc6(LX_futex, &word, 99, 0, 0, 0, 0) == -38, 0);

    /* --- random, sockets --- */
    unsigned char rnd[32] = { 0 };
    CHECKV(sc3(LX_getrandom, rnd, 32, 0) == 32, 0);
    int zero = 1;
    for (int i = 0; i < 32; i++)
        if (rnd[i])
            zero = 0;
    CHECK(!zero);
    long s = sc3(LX_socket, LX_AF_INET, LX_SOCK_DGRAM | LX_SOCK_CLOEXEC, 0);
    CHECKV(s >= 3, s);
    struct lx_sockaddr_in me = { .sin_family = LX_AF_INET, .sin_port = (uint16_t)((40100 >> 8) | (40100 << 8)),
                                 .sin_addr = 0x0100007f };
    CHECKV(sc3(LX_bind, s, &me, sizeof(me)) == 0, 0);
    CHECKV(sc6(LX_sendto, s, "ping", 4, 0, &me, sizeof(me)) == 4, 0);
    struct lx_sockaddr_in from;
    int32_t flen = sizeof(from);
    CHECKV(sc6(LX_recvfrom, s, buf, 16, 0, &from, &flen) == 4 && memeq(buf, "ping", 4), 0);
    CHECKV(flen == 16 && from.sin_family == LX_AF_INET && from.sin_port == me.sin_port && from.sin_addr == 0x0100007f, flen);
    int32_t short_len = 4;
    struct { struct lx_sockaddr_in a; uint32_t canary; } box = { .canary = 0xdeadbeef };
    CHECKV(sc3(LX_getsockname, s, &box.a, &short_len) == 0 && short_len == 16 && box.canary == 0xdeadbeef, short_len);
    CHECKV(sc2(LX_fstat, s, &st) == 0 && (st.st_mode & LX_S_IFMT) == LX_S_IFSOCK, 0);
    int one = 1;
    CHECKV(sc6(LX_setsockopt, s, LX_SOL_SOCKET, 2, &one, 4, 0) == 0, 0);   /* SO_REUSEADDR: accepted */
    CHECKV(sc1(LX_close, s) == 0, 0);
    CHECKV(sc3(LX_socket, 1, LX_SOCK_STREAM, 0) == -97, 0);   /* AF_UNIX: EAFNOSUPPORT */

    /* --- unknown numbers --- */
    CHECKV(sc0(510) == -38, 0);
    CHECKV(sc0(9999) == -38, 0);

    if (g_failures == 0) {
        lx_puts("LINUXTEST: PASS\n");
        return 0;
    }
    lx_puts("LINUXTEST: FAIL (");
    put_num(g_failures);
    lx_puts(" checks)\n");
    return 1;
}
