/*
 * init.c - The first user program.
 *
 * Without arguments it announces itself and exits; this phase has
 * nothing for it to supervise yet. With --selftest it exercises every
 * system call with valid, invalid, boundary, and hostile inputs and
 * reports USERTEST: PASS or FAIL; the kernel self-test runs it that way
 * and checks the exit status. With --crash it dereferences address 0 so
 * the kernel's fatal-fault path can be tested.
 */

#include <stddef.h>
#include <stdint.h>

#include <cosmo/syscall.h>

static size_t ustrlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

static int ustrcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void puts_h(int h, const char *s)
{
    cosmo_write(h, s, ustrlen(s));
}

static void put_num(int h, long v)
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
    puts_h(h, buf + i);
}

static int g_failures;

static void check(int cond, const char *what)
{
    if (cond)
        return;
    g_failures++;
    puts_h(2, "USERTEST: check failed: ");
    puts_h(2, what);
    puts_h(2, "\n");
}

#define CHECK(c) check((c), #c)


static int umemcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i])
            return x[i] - y[i];
    }
    return 0;
}

/* Phase 7: the filesystem calls on ramfs, then the scratch disk. */
static void fs_selftest(void)
{
    struct cosmo_stat st;
    char buf[256];

    /* /boot was populated from the boot archive. */
    CHECK(cosmo_stat("/boot/init", &st) == 0 && st.type == COSMO_DT_REG && st.size > 1000);
    CHECK(cosmo_stat("/boot", &st) == 0 && st.type == COSMO_DT_DIR);
    CHECK(cosmo_stat("/nope", &st) == -COSMO_ENOENT);
    CHECK(cosmo_stat("/boot/init/x", &st) == -COSMO_ENOTDIR);

    /* Create, write, seek, read, fstat. */
    long h = cosmo_open("/tmp/usertest.txt", COSMO_O_RDWR | COSMO_O_CREAT | COSMO_O_TRUNC, 0644);
    CHECK(h >= 3);
    CHECK(cosmo_write((int)h, "hello, filesystem\n", 18) == 18);
    CHECK(cosmo_fstat((int)h, &st) == 0 && st.size == 18 && st.type == COSMO_DT_REG);
    CHECK(cosmo_lseek((int)h, 0, COSMO_SEEK_SET) == 0);
    CHECK(cosmo_read((int)h, buf, sizeof(buf)) == 18 && umemcmp(buf, "hello, filesystem\n", 18) == 0);
    CHECK(cosmo_read((int)h, buf, sizeof(buf)) == 0);                 /* EOF */
    CHECK(cosmo_lseek((int)h, 7, COSMO_SEEK_SET) == 7);
    CHECK(cosmo_read((int)h, buf, 10) == 10 && umemcmp(buf, "filesystem", 10) == 0);
    CHECK(cosmo_lseek((int)h, -1, COSMO_SEEK_SET) == -COSMO_EINVAL);
    CHECK(cosmo_lseek((int)h, 0, COSMO_SEEK_END) == 18);
    CHECK(cosmo_close((int)h) == 0);
    CHECK(cosmo_close((int)h) == -COSMO_EBADF);

    /* Access modes are enforced through handle rights. */
    h = cosmo_open("/tmp/usertest.txt", COSMO_O_RDONLY, 0);
    CHECK(h >= 3);
    CHECK(cosmo_write((int)h, "x", 1) == -COSMO_EBADF);
    CHECK(cosmo_read((int)h, buf, 5) == 5);
    CHECK(cosmo_close((int)h) == 0);
    CHECK(cosmo_open("/tmp/usertest.txt", COSMO_O_RDWR | COSMO_O_CREAT | COSMO_O_EXCL, 0644) == -COSMO_EEXIST);
    CHECK(cosmo_open("/tmp/missing", COSMO_O_RDONLY, 0) == -COSMO_ENOENT);
    CHECK(cosmo_open("/tmp", COSMO_O_WRONLY, 0) == -COSMO_EISDIR);
    CHECK(cosmo_open((const char *)0x10, COSMO_O_RDONLY, 0) == -COSMO_EFAULT);

    /* Directories: mkdir, rename, getdents, unlink, rmdir. */
    CHECK(cosmo_mkdir("/tmp/d", 0755) == 0);
    CHECK(cosmo_mkdir("/tmp/d", 0755) == -COSMO_EEXIST);
    CHECK(cosmo_rename("/tmp/usertest.txt", "/tmp/d/moved.txt") == 0);
    CHECK(cosmo_stat("/tmp/usertest.txt", &st) == -COSMO_ENOENT);
    CHECK(cosmo_stat("/tmp/d/moved.txt", &st) == 0 && st.size == 18);
    h = cosmo_open("/tmp/d", COSMO_O_RDONLY | COSMO_O_DIRECTORY, 0);
    CHECK(h >= 3);
    long n = cosmo_getdents((int)h, buf, sizeof(buf));
    CHECK(n > 0);
    int seen = 0;
    for (long off = 0; off < n;) {
        struct cosmo_dirent *d = (struct cosmo_dirent *)(buf + off);
        if (ustrcmp(d->name, "moved.txt") == 0 && d->type == COSMO_DT_REG)
            seen |= 1;
        if (ustrcmp(d->name, ".") == 0)
            seen |= 2;
        if (ustrcmp(d->name, "..") == 0)
            seen |= 4;
        off += d->reclen;
    }
    CHECK(seen == 7);
    CHECK(cosmo_getdents((int)h, buf, sizeof(buf)) == 0);              /* end */
    CHECK(cosmo_close((int)h) == 0);
    CHECK(cosmo_rmdir("/tmp/d") == -COSMO_ENOTEMPTY);
    CHECK(cosmo_unlink("/tmp/d") == -COSMO_EISDIR);
    CHECK(cosmo_unlink("/tmp/d/moved.txt") == 0);
    CHECK(cosmo_rmdir("/tmp/d") == 0);
    CHECK(cosmo_stat("/tmp/d", &st) == -COSMO_ENOENT);
    CHECK(cosmo_sync() == 0);

    /* The kernel self-test left a cosmofs on the scratch disk with a
     * file in it; mount it and read it back from user mode. */
    long m = cosmo_mount("vda", "/mnt", "cosmofs", 0);
    if (m == 0) {
        h = cosmo_open("/mnt/hello.txt", COSMO_O_RDONLY, 0);
        CHECK(h >= 3);
        n = cosmo_read((int)h, buf, sizeof(buf));
        CHECK(n == 21 && umemcmp(buf, "hello from the kernel", 21) == 0);
        CHECK(cosmo_close((int)h) == 0);
        CHECK(cosmo_stat("/mnt/dir/nested.txt", &st) == 0 && st.type == COSMO_DT_REG);
        CHECK(cosmo_umount("/mnt") == 0);
        CHECK(cosmo_stat("/mnt/hello.txt", &st) == -COSMO_ENOENT);
        puts_h(1, "usertest: cosmofs mounted and read from user mode\n");
    } else {
        CHECK(m == -COSMO_ENODEV || m == -COSMO_EIO);   /* no disk, or not formatted yet */
        puts_h(1, "usertest: no cosmofs to mount\n");
    }
    CHECK(cosmo_umount("/") == -COSMO_EBUSY);
}

static void selftest(void)
{
    fs_selftest();
    /* --- write --- */
    CHECK(cosmo_write(1, "usertest: write ok\n", 19) == 19);
    CHECK(cosmo_write(1, "", 0) == 0);
    CHECK(cosmo_write(7, "x", 1) == -COSMO_EBADF);                 /* unopened handle */
    CHECK(cosmo_write(0, "x", 1) == -COSMO_EBADF);                 /* stdin lacks WRITE */
    CHECK(cosmo_write(-1, "x", 1) == -COSMO_EBADF);
    CHECK(cosmo_write(1, (void *)0xffffffff80000000ULL, 1) == -COSMO_EFAULT); /* kernel pointer */
    CHECK(cosmo_write(1, (void *)0x10, 1) == -COSMO_EFAULT);        /* below the user window */
    CHECK(cosmo_write(1, (void *)0x00007FFFFFFFF000ULL, 1) == -COSMO_EFAULT); /* top of window */
    CHECK(cosmo_write(1, (void *)0x0000600000000000ULL, 1) == -COSMO_EFAULT); /* unmapped */
    CHECK(cosmo_write(1, "abc", (size_t)-1) == -COSMO_EFAULT);     /* length overflow */

    /* --- read --- */
    char rb[8];
    CHECK(cosmo_read(0, rb, sizeof(rb)) == 0);                      /* console: EOF for now */
    CHECK(cosmo_read(1, rb, sizeof(rb)) == -COSMO_EBADF);           /* stdout lacks READ */
    CHECK(cosmo_read(0, (void *)0xffffffff80000000ULL, 8) == -COSMO_EFAULT);

    /* --- pid, yield, clock, sleep --- */
    long pid = cosmo_getpid();
    CHECK(pid > 0);
    CHECK(cosmo_yield() == 0);
    uint64_t t0 = cosmo_clock_ns();
    CHECK(cosmo_sleep_ns(5000000) == 0);                            /* 5 ms */
    uint64_t t1 = cosmo_clock_ns();
    CHECK(t1 >= t0 + 5000000);
    CHECK(t1 - t0 < 200000000);
    CHECK(cosmo_sleep_ns(4000ULL * 1000000000ULL) == -COSMO_EINVAL); /* > 1 h refused */

    /* --- mmap / munmap --- */
    long m = cosmo_mmap(NULL, 3 * 4096, COSMO_PROT_READ | COSMO_PROT_WRITE, COSMO_MAP_ANONYMOUS);
    CHECK(m > 0);
    if (m > 0) {
        volatile uint32_t *p = (volatile uint32_t *)m;
        CHECK(p[0] == 0 && p[3 * 1024 - 1] == 0);                     /* demand-zero */
        p[0] = 0x11223344;
        p[3 * 1024 - 1] = 0x55667788;
        CHECK(p[0] == 0x11223344 && p[3 * 1024 - 1] == 0x55667788);
        CHECK(cosmo_write(1, (const void *)m, 0) == 0);
        CHECK(cosmo_munmap((void *)m, 3 * 4096) == 0);
        CHECK(cosmo_munmap((void *)m, 3 * 4096) == -COSMO_EINVAL);   /* already gone */
    }
    CHECK(cosmo_mmap(NULL, 0, COSMO_PROT_READ, COSMO_MAP_ANONYMOUS) == -COSMO_EINVAL);
    CHECK(cosmo_mmap(NULL, 4096 + 1, COSMO_PROT_READ, COSMO_MAP_ANONYMOUS) == -COSMO_EINVAL);
    CHECK(cosmo_mmap(NULL, 4096, COSMO_PROT_READ | COSMO_PROT_WRITE | COSMO_PROT_EXEC,
                     COSMO_MAP_ANONYMOUS) == -COSMO_EINVAL);           /* W^X */
    CHECK(cosmo_mmap(NULL, 4096, COSMO_PROT_READ, 0) == -COSMO_EINVAL); /* not anonymous */
    CHECK(cosmo_mmap((void *)0x10, 4096, COSMO_PROT_READ, COSMO_MAP_ANONYMOUS | COSMO_MAP_FIXED) ==
          -COSMO_EINVAL);
    long fx = cosmo_mmap((void *)0x0000200000000000ULL, 4096, COSMO_PROT_READ | COSMO_PROT_WRITE,
                         COSMO_MAP_ANONYMOUS | COSMO_MAP_FIXED);
    CHECK(fx == 0x0000200000000000L);
    if (fx > 0) {
        *(volatile char *)fx = 'z';
        CHECK(cosmo_mmap((void *)fx, 4096, COSMO_PROT_READ, COSMO_MAP_ANONYMOUS | COSMO_MAP_FIXED) ==
              -COSMO_EEXIST);                                         /* overlap */
        CHECK(cosmo_munmap((void *)fx, 4096) == 0);
    }
    CHECK(cosmo_munmap((void *)0x10, 4096) == -COSMO_EINVAL);

    /* --- log, close, unknown numbers --- */
    CHECK(cosmo_log("hello from user mode", 20) == 0);
    CHECK(cosmo_log((const char *)0xffffffff80000000ULL, 5) == -COSMO_EFAULT);
    CHECK(cosmo_log("x", 4096) == -COSMO_EINVAL);
    CHECK(cosmo_close(7) == -COSMO_EBADF);
    CHECK(cosmo_close(2) == 0);
    CHECK(cosmo_write(2, "x", 1) == -COSMO_EBADF);                 /* closed */
    CHECK(cosmo_syscall0(SYS_COUNT) == -COSMO_ENOSYS);
    CHECK(cosmo_syscall0(999999) == -COSMO_ENOSYS);
    CHECK(cosmo_syscall0(-1) == -COSMO_ENOSYS);

    /* --- stack: deep use faults in lazily populated pages --- */
    volatile char big[64 * 1024];
    big[0] = 1;
    big[sizeof(big) - 1] = 2;
    CHECK(big[0] == 1 && big[sizeof(big) - 1] == 2);
}

int main(int argc, char **argv, char **envp)
{
    (void)envp;

    if (argc >= 2 && ustrcmp(argv[1], "--crash") == 0) {
        puts_h(1, "init: crashing on purpose\n");
        *(volatile int *)0 = 1;
        return 7; /* not reached */
    }

    if (argc >= 2 && ustrcmp(argv[1], "--selftest") == 0) {
        selftest();
        if (g_failures == 0) {
            puts_h(1, "USERTEST: PASS\n");
            return 0;
        }
        puts_h(1, "USERTEST: FAIL (");
        put_num(1, g_failures);
        puts_h(1, " checks)\n");
        return 1;
    }

    puts_h(1, "init: hello from user mode, pid ");
    put_num(1, cosmo_getpid());
    puts_h(1, ", argc ");
    put_num(1, argc);
    puts_h(1, "\n");
    puts_h(1, "init: nothing to supervise yet; exiting\n");
    return 0;
}
