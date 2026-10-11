/*
 * lxpty.c - pseudo-terminals, sessions and job control through the Linux
 * ABI (roadmap M4; docs/compat/linux/testing.md). Freestanding, raw system
 * calls, run by rc.linux; prints "lxpty: ok" or the first failed check.
 *
 * Part 1, one process: the path musl's posix_openpt, grantpt, unlockpt
 * and ptsname take (open /dev/ptmx O_NOCTTY, TIOCGPTN, TIOCSPTLCK), the
 * slave locked until unlocked, the default modes (TCGETS: ICANON, ECHO,
 * ISIG, OPOST|ONLCR), a line typed at the master read whole from the slave
 * and echoed with CR LF, slave output's newline as CR LF, raw mode
 * (TCSETS), the window size (TIOCSWINSZ/TIOCGWINSZ), FIONREAD, and the
 * master's read -EIO once every slave open has closed.
 *
 * Part 2, a session on the slave: a child C calls setsid and TIOCSCTTY;
 * the slave is its controlling terminal (TIOCGSID, TIOCGPGRP). C runs
 * jobs in groups of their own and moves the foreground (TIOCSPGRP); this
 * process, outside the session, types at the master:
 *   ^C to a foreground job: it dies of SIGINT;
 *   ^Z to a foreground job: wait4(WUNTRACED) sees it stopped by SIGTSTP,
 *     SIGCONT and wait4(WCONTINUED) see it continued;
 *   a background job that reads the terminal is stopped by SIGTTIN;
 *   with TOSTOP a background job that writes is stopped by SIGTTOU, and
 *     without it the write goes through;
 *   a window-size change raises SIGWINCH on the foreground job;
 *   closing the master hangs the session up: C, its leader, dies of SIGHUP.
 * C reports a failed check through its exit status; the hangup is the one
 * way it may end for a pass.
 */

#include "lxabi.h"

#define SIGHUP_ 1
#define SIGINT_ 2
#define SIGKILL_ 9
#define SIGCONT_ 18
#define SIGTSTP_ 20
#define SIGTTIN_ 21
#define SIGTTOU_ 22
#define SIGWINCH_ 28

static void fail(const char *what, long v)
{
    char buf[160];
    size_t n = 0;
    const char *p = "lxpty: FAIL ";
    while (*p)
        buf[n++] = *p++;
    for (p = what; *p && n < 120; p++)
        buf[n++] = *p;
    buf[n++] = ' ';
    if (v < 0) {
        buf[n++] = '-';
        v = -v;
    }
    char d[24];
    int k = 0;
    do {
        d[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && k < 20);
    while (k)
        buf[n++] = d[--k];
    buf[n++] = '\n';
    sc3(LX_write, 1, buf, n);
    lx_exit(1);
}
#define CHECK(c, v)                                                                                         \
    do {                                                                                                    \
        if (!(c))                                                                                           \
            fail(#c, (long)(v));                                                                            \
    } while (0)
/* In the session child: the number of the check is its exit status. */
#define CCHECK(c, n)                                                                                        \
    do {                                                                                                    \
        if (!(c))                                                                                           \
            lx_exit(n);                                                                                     \
    } while (0)

static long xopen(const char *path, long flags) { return sc4(LX_openat, LX_AT_FDCWD, path, flags, 0); }

/* fork: clone with SIGCHLD and no new stack, the same on both architectures. */
static long xfork(void) { return sc6(LX_clone, LX_CLONE_EXIT_SIGCHLD, 0, 0, 0, 0, 0); }

static int memeq(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i])
            return 0;
    return 1;
}

static void nap_ms(long ms)
{
    struct lx_timespec t = { 0, ms * 1000000L };
    sc2(LX_nanosleep, &t, 0);
}

/* Read exactly `want` bytes from the master (the echo and the output
 * arrive as the slave side produces them), a second at most. */
static long read_master(int m, char *buf, long want)
{
    long got = 0;
    for (int i = 0; i < 100 && got < want; i++) {
        uint32_t q = 0;
        if (sc3(LX_ioctl, m, LX_FIONREAD, &q) == 0 && q > 0) {
            long n = sc3(LX_read, m, buf + got, want - got);
            if (n < 0)
                return n;
            got += n;
        } else {
            nap_ms(10);
        }
    }
    return got;
}

static int wstopsig(int st) { return (st & 0xff) == 0x7f ? (st >> 8) & 0xff : 0; }
static int wtermsig(int st) { return (st & 0x7f) != 0 && (st & 0x7f) != 0x7f ? st & 0x7f : 0; }
static int wexited(int st) { return (st & 0x7f) == 0; }

/* --- part 2: the session child ------------------------------------------------ */

/* A job: a child in a group of its own, foreground or not, that does
 * `what` on the terminal. */
enum job { J_READ, J_WRITE, J_WINCH };

/* The pipe a foreground job waits on until the shell has given it the
 * terminal: a read before that is a background read, SIGTTIN. */
static int g_jobgo[2];

static long job(int s, enum job what, int fg, int sync_w)
{
    long pid = xfork();
    if (pid == 0) {
        sc2(LX_setpgid, 0, 0);
        uint64_t ttou = 1ULL << (SIGTTOU_ - 1);
        sc4(LX_rt_sigprocmask, 1 /* SIG_UNBLOCK */, &ttou, 0, 8);   /* the shell's block is not the job's */
        if (what == J_WINCH) {
            uint64_t mask = 1ULL << (SIGWINCH_ - 1);
            sc4(LX_rt_sigprocmask, 0 /* SIG_BLOCK */, &mask, 0, 8);
        }
        char r = 'r';
        sc3(LX_write, sync_w, &r, 1);   /* ready: its group exists, the signal is blocked */
        if (fg)
            sc3(LX_read, g_jobgo[0], &r, 1);   /* the terminal is this group's now */
        char b[64];
        if (what == J_READ) {
            long n = sc3(LX_read, s, b, sizeof(b));
            lx_exit(n >= 0 ? 0 : 3);
        }
        if (what == J_WRITE) {
            long n = sc3(LX_write, s, "bg\n", 3);
            lx_exit(n == 3 ? 0 : 4);
        }
        for (int i = 0; i < 200; i++) {   /* J_WINCH: wait for it to be pending, 2 s at most */
            uint64_t pend = 0;
            sc2(LX_rt_sigpending, &pend, 8);
            if (pend & (1ULL << (SIGWINCH_ - 1)))
                lx_exit(0);
            nap_ms(10);
        }
        lx_exit(5);
    }
    if (pid > 0)
        sc2(LX_setpgid, pid, pid);   /* both sides set it: neither order can miss */
    return pid;
}

static void wait_ready(int sync_r)
{
    char c;
    sc3(LX_read, sync_r, &c, 1);
}

static void tell(int ctl_w, char c) { sc3(LX_write, ctl_w, &c, 1); }

/* The shell's half of starting a foreground job: the terminal to its
 * group, then the job may go. */
static int give_terminal(int s, long j)
{
    int32_t v = (int32_t)j;
    if (sc3(LX_ioctl, s, LX_TIOCSPGRP, &v) != 0)
        return 0;
    char g = 'g';
    sc3(LX_write, g_jobgo[1], &g, 1);
    return 1;
}

static void session_child(int s, int sync_r, int sync_w, int ctl_w, int go_r)
{
    long me = sc0(LX_getpid);
    CCHECK(sc2(LX_pipe2, g_jobgo, 0) == 0, 9);
    CCHECK(sc0(LX_setsid) == me, 10);
    CCHECK(sc3(LX_ioctl, s, LX_TIOCSCTTY, 0) == 0, 11);
    int32_t v = 0;
    CCHECK(sc3(LX_ioctl, s, LX_TIOCGSID, &v) == 0 && v == me, 12);
    CCHECK(sc3(LX_ioctl, s, LX_TIOCGPGRP, &v) == 0 && v == me, 13);
    CCHECK(sc3(LX_ioctl, s, LX_TIOCSCTTY, 0) == 0, 14);   /* its own already: no change */
    /* Like a shell, this one blocks SIGTTOU: it moves the foreground from
     * the background (after a job ends or stops), which would stop it. */
    uint64_t ttou = 1ULL << (SIGTTOU_ - 1);
    sc4(LX_rt_sigprocmask, 0 /* SIG_BLOCK */, &ttou, 0, 8);
    char go;
    int st;

    /* ^C: the foreground job dies of SIGINT. */
    long j = job(s, J_READ, 1, sync_w);
    CCHECK(j > 0, 20);
    wait_ready(sync_r);
    CCHECK(give_terminal(s, j), 21);
    tell(ctl_w, 'C');                     /* the parent types ^C */
    CCHECK(sc4(LX_wait4, j, &st, 0, 0) == j && wtermsig(st) == SIGINT_, 22);

    /* ^Z: stopped by SIGTSTP; SIGCONT continues it. */
    j = job(s, J_READ, 1, sync_w);
    CCHECK(j > 0, 30);
    wait_ready(sync_r);
    CCHECK(give_terminal(s, j), 31);
    tell(ctl_w, 'Z');
    CCHECK(sc4(LX_wait4, j, &st, LX_WUNTRACED, 0) == j && wstopsig(st) == SIGTSTP_, 32);
    CCHECK(sc2(LX_kill, j, SIGCONT_) == 0, 33);
    CCHECK(sc4(LX_wait4, j, &st, LX_WCONTINUED, 0) == j && st == 0xffff, 34);
    sc2(LX_kill, j, SIGKILL_);
    CCHECK(sc4(LX_wait4, j, &st, 0, 0) == j && wtermsig(st) == SIGKILL_, 35);

    /* The foreground back to this group, from the background. */
    v = (int32_t)me;
    CCHECK(sc3(LX_ioctl, s, LX_TIOCSPGRP, &v) == 0, 40);

    /* A background reader: SIGTTIN. */
    j = job(s, J_READ, 0, sync_w);
    CCHECK(j > 0, 41);
    wait_ready(sync_r);
    CCHECK(sc4(LX_wait4, j, &st, LX_WUNTRACED, 0) == j && wstopsig(st) == SIGTTIN_, 42);
    sc2(LX_kill, j, SIGKILL_);
    sc4(LX_wait4, j, &st, 0, 0);

    /* TOSTOP: a background writer is stopped by SIGTTOU; without it, writes. */
    struct lx_termios tio;
    CCHECK(sc3(LX_ioctl, s, LX_TCGETS, &tio) == 0, 50);
    tio.c_lflag |= LX_TOSTOP;
    CCHECK(sc3(LX_ioctl, s, LX_TCSETS, &tio) == 0, 51);
    j = job(s, J_WRITE, 0, sync_w);
    CCHECK(j > 0, 52);
    wait_ready(sync_r);
    CCHECK(sc4(LX_wait4, j, &st, LX_WUNTRACED, 0) == j && wstopsig(st) == SIGTTOU_, 53);
    sc2(LX_kill, j, SIGKILL_);
    sc4(LX_wait4, j, &st, 0, 0);
    tio.c_lflag &= ~(uint32_t)LX_TOSTOP;
    CCHECK(sc3(LX_ioctl, s, LX_TCSETS, &tio) == 0, 54);
    j = job(s, J_WRITE, 0, sync_w);
    CCHECK(j > 0, 55);
    wait_ready(sync_r);
    CCHECK(sc4(LX_wait4, j, &st, 0, 0) == j && wexited(st) && ((st >> 8) & 0xff) == 0, 56);

    /* SIGWINCH to the foreground job when the size changes. */
    j = job(s, J_WINCH, 1, sync_w);
    CCHECK(j > 0, 60);
    wait_ready(sync_r);
    CCHECK(give_terminal(s, j), 61);
    struct lx_winsize ws = { .ws_row = 33, .ws_col = 99 };
    CCHECK(sc3(LX_ioctl, s, LX_TIOCSWINSZ, &ws) == 0, 62);
    CCHECK(sc4(LX_wait4, j, &st, 0, 0) == j && wexited(st) && ((st >> 8) & 0xff) == 0, 63);
    v = (int32_t)me;
    CCHECK(sc3(LX_ioctl, s, LX_TIOCSPGRP, &v) == 0, 64);

    /* The hangup: the parent closes the master; this leader dies of SIGHUP. */
    tell(ctl_w, 'H');
    sc3(LX_read, go_r, &go, 1);   /* blocks; the hangup ends it */
    lx_exit(70);                  /* the parent closed the pipe and not the master */
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    /* --- part 1 --- */
    long m = xopen("/dev/ptmx", LX_O_RDWR | LX_O_NOCTTY);
    CHECK(m >= 0, m);
    uint32_t n = 99;
    CHECK(sc3(LX_ioctl, m, LX_TIOCGPTN, &n) == 0 && n < 64, n);
    char path[] = "/dev/pts/00";
    if (n < 10) {
        path[9] = (char)('0' + n);
        path[10] = '\0';
    } else {
        path[9] = (char)('0' + n / 10);
        path[10] = (char)('0' + n % 10);
    }
    int32_t lock = -1;
    CHECK(sc3(LX_ioctl, m, LX_TIOCGPTLCK, &lock) == 0 && lock == 1, lock);
    CHECK(xopen(path, LX_O_RDWR | LX_O_NOCTTY) == -5, 0);   /* EIO: locked until unlockpt */
    lock = 0;
    CHECK(sc3(LX_ioctl, m, LX_TIOCSPTLCK, &lock) == 0, 0);
    long s = xopen(path, LX_O_RDWR | LX_O_NOCTTY);
    CHECK(s >= 0, s);
    CHECK(sc3(LX_ioctl, s, LX_TIOCGPTN, &n) == -25, 0);   /* ENOTTY: a slave has no number to give */
    /* What musl's ttyname does: /proc/self/fd/N names the slave, and the
     * name's inode is the open file's. */
    {
        char link[] = "/proc/self/fd/00";
        link[14] = (char)('0' + s / 10);
        link[15] = (char)('0' + s % 10);
        if (s < 10) {
            link[14] = (char)('0' + s);
            link[15] = '\0';
        }
        char target[64];
        long tl = sc4(LX_readlinkat, LX_AT_FDCWD, link, target, sizeof(target));
        CHECK(tl == (long)lx_strlen(path) && memeq(target, path, (size_t)tl), tl);
        struct lx_stat a, bst;
        CHECK(sc4(LX_newfstatat, LX_AT_FDCWD, path, &a, 0) == 0 && sc2(LX_fstat, s, &bst) == 0, 0);
        CHECK(a.st_ino == bst.st_ino && a.st_dev == bst.st_dev, a.st_ino);
    }
    struct lx_termios tio;
    CHECK(sc3(LX_ioctl, s, LX_TCGETS, &tio) == 0, 0);
    CHECK((tio.c_lflag & (LX_ICANON | LX_ECHO | LX_ISIG)) == (LX_ICANON | LX_ECHO | LX_ISIG), tio.c_lflag);
    CHECK((tio.c_oflag & (LX_OPOST | LX_ONLCR)) == (LX_OPOST | LX_ONLCR), tio.c_oflag);
    CHECK(tio.c_cc[LX_VINTR] == 3 && tio.c_cc[LX_VSUSP] == 0x1a, tio.c_cc[LX_VINTR]);
    CHECK(sc3(LX_ioctl, m, LX_TCGETS, &tio) == 0, 0);   /* the master answers for the pair */

    /* A line typed at the master: read whole from the slave, echoed CR LF. */
    CHECK(sc3(LX_write, m, "hi\n", 3) == 3, 0);
    char b[64];
    long r = sc3(LX_read, s, b, sizeof(b));
    CHECK(r == 3 && memeq(b, "hi\n", 3), r);
    r = read_master((int)m, b, 4);
    CHECK(r == 4 && memeq(b, "hi\r\n", 4), r);
    /* Output: a newline goes out as CR LF. */
    CHECK(sc3(LX_write, s, "a\nb", 3) == 3, 0);
    r = read_master((int)m, b, 4);
    CHECK(r == 4 && memeq(b, "a\r\nb", 4), r);
    /* The output queue, from both ends: the slave's unread output is its
     * TIOCOUTQ and the master's FIONREAD, and the slave's TCOFLUSH drops
     * it (the master's TCIFLUSH names the same queue). */
    CHECK(sc3(LX_write, s, "abc", 3) == 3, 0);
    uint32_t q = 0;
    CHECK(sc3(LX_ioctl, s, LX_TIOCOUTQ, &q) == 0 && q == 3, q);
    CHECK(sc3(LX_ioctl, m, LX_FIONREAD, &q) == 0 && q == 3, q);
    CHECK(sc3(LX_ioctl, s, LX_TCFLSH, LX_TCOFLUSH) == 0, 0);
    CHECK(sc3(LX_ioctl, m, LX_FIONREAD, &q) == 0 && q == 0, q);
    CHECK(sc3(LX_write, s, "d", 1) == 1 && sc3(LX_ioctl, m, LX_TCFLSH, LX_TCIFLUSH) == 0, 0);
    CHECK(sc3(LX_ioctl, s, LX_TIOCOUTQ, &q) == 0 && q == 0, q);
    /* Raw: bytes as typed, no echo, no line. */
    CHECK(sc3(LX_ioctl, s, LX_TCGETS, &tio) == 0, 0);
    struct lx_termios raw = tio;
    raw.c_lflag &= ~(uint32_t)(LX_ICANON | LX_ECHO);
    raw.c_cc[LX_VMIN] = 1;
    CHECK(sc3(LX_ioctl, s, LX_TCSETSF, &raw) == 0, 0);
    CHECK(sc3(LX_write, m, "xyz", 3) == 3, 0);
    r = sc3(LX_read, s, b, sizeof(b));
    CHECK(r >= 1 && b[0] == 'x', r);
    q = 7;
    CHECK(sc3(LX_ioctl, m, LX_FIONREAD, &q) == 0 && q == 0, q);   /* nothing echoed */
    CHECK(sc3(LX_ioctl, s, LX_TCSETS, &tio) == 0, 0);
    /* The window size, set on either side, read on the other. */
    struct lx_winsize ws = { .ws_row = 24, .ws_col = 80 }, got = { 0 };
    CHECK(sc3(LX_ioctl, m, LX_TIOCSWINSZ, &ws) == 0, 0);
    CHECK(sc3(LX_ioctl, s, LX_TIOCGWINSZ, &got) == 0 && got.ws_row == 24 && got.ws_col == 80, got.ws_row);

    /* A second pair: the slave's last close makes the master's read EIO. */
    long m2 = xopen("/dev/ptmx", LX_O_RDWR | LX_O_NOCTTY);
    CHECK(m2 >= 0, m2);
    uint32_t n2 = 0;
    CHECK(sc3(LX_ioctl, m2, LX_TIOCGPTN, &n2) == 0 && n2 != n, n2);
    lock = 0;
    sc3(LX_ioctl, m2, LX_TIOCSPTLCK, &lock);
    char path2[] = "/dev/pts/00";
    path2[9] = (char)('0' + n2 / 10 % 10);
    path2[10] = (char)('0' + n2 % 10);
    if (n2 < 10) {
        path2[9] = (char)('0' + n2);
        path2[10] = '\0';
    }
    long s2 = xopen(path2, LX_O_RDWR | LX_O_NOCTTY);
    CHECK(s2 >= 0, s2);
    sc1(LX_close, s2);
    CHECK(sc3(LX_read, m2, b, sizeof(b)) == -5, 0);
    sc1(LX_close, m2);
    CHECK(xopen(path2, LX_O_RDWR | LX_O_NOCTTY) == -2, 0);   /* ENOENT: the name went with the master */
    lx_puts("lxpty: pty ok\n");

    /* --- part 2 --- */
    int sync[2], ctl[2], go[2];
    CHECK(sc2(LX_pipe2, sync, 0) == 0 && sc2(LX_pipe2, ctl, 0) == 0 && sc2(LX_pipe2, go, 0) == 0, 0);
    long c = xfork();
    CHECK(c >= 0, c);
    if (c == 0) {
        sc1(LX_close, m);   /* the master is the parent's alone, or its close would not hang up */
        session_child((int)s, sync[0], sync[1], ctl[1], go[0]);
    }
    sc1(LX_close, s);
    /* Only the child writes ctl and uses sync, so its exit ends this side's
     * read; go stays open here, or the child's read of it would end. */
    sc1(LX_close, ctl[1]);
    sc1(LX_close, sync[0]);
    sc1(LX_close, sync[1]);
    sc1(LX_close, go[0]);
    char cmd = 0;
    int st = 0;
    for (;;) {
        if (sc3(LX_read, ctl[0], &cmd, 1) != 1) {
            CHECK(sc4(LX_wait4, c, &st, 0, 0) == c, 0);
            fail("the session child ended early, status", st);
        }
        if (cmd == 'C')
            sc3(LX_write, m, "\x03", 1);
        else if (cmd == 'Z')
            sc3(LX_write, m, "\x1a", 1);
        else if (cmd == 'H')
            break;
    }
    nap_ms(20);   /* the child is in its read of the go pipe */
    sc1(LX_close, m);
    CHECK(sc4(LX_wait4, c, &st, 0, 0) == c, 0);
    CHECK(wtermsig(st) == SIGHUP_, st);
    lx_puts("lxpty: jobs ok\n");
    lx_puts("lxpty: ok\n");
    lx_exit(0);
    return 0;
}

