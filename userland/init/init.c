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

static void selftest(void)
{
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
