/*
 * ttytest.c - Self-test of the line discipline on a private tty
 * (docs/kernel/tty/testing.md). Echo is off so the log stays clean.
 */

#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/selftest.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/tty.h>
#include <kernel/wait.h>

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            *reason = "tty: " #cond;                                                         \
            return false;                                                                    \
        }                                                                                    \
    } while (0)

static void feed(struct tty *t, const char *s)
{
    tty_input(t, (const uint8_t *)s, strlen(s));
}

struct reader {
    struct tty *tty;
    char buf[64];
    int64_t got;
    volatile bool done;
};

static void reader_thread(void *arg)
{
    struct reader *r = arg;
    r->got = tty_read(r->tty, r->buf, sizeof(r->buf));
    r->done = true;
    thread_exit(0);
}

bool selftest_tty_ldisc(const char **reason)
{
    static struct tty t;   /* 5 KiB: not on the stack */
    tty_setup(&t, "test");
    t.flags &= ~TTY_ECHO;
    char buf[64];

    /* Erase, kill, newline. */
    feed(&t, "abc\x7f" "d\n");
    CHECK(tty_read(&t, buf, sizeof(buf)) == 4 && memcmp(buf, "abd\n", 4) == 0);
    feed(&t, "xyz\x15q\n");
    CHECK(tty_read(&t, buf, sizeof(buf)) == 2 && memcmp(buf, "q\n", 2) == 0);
    feed(&t, "\bnothing to erase\b\b\b\b\b\b\b\b\n");
    CHECK(tty_read(&t, buf, sizeof(buf)) == 9 && memcmp(buf, "nothing \n", 9) == 0);

    /* CR becomes NL; two lines are two reads. */
    feed(&t, "a\rb\r");
    CHECK(tty_read(&t, buf, sizeof(buf)) == 2 && buf[0] == 'a');
    CHECK(tty_read(&t, buf, sizeof(buf)) == 2 && buf[0] == 'b');

    /* A small buffer takes a prefix; the rest follows with its newline. */
    feed(&t, "hello\n");
    CHECK(tty_read(&t, buf, 3) == 3 && memcmp(buf, "hel", 3) == 0);
    CHECK(tty_read(&t, buf, sizeof(buf)) == 3 && memcmp(buf, "lo\n", 3) == 0);

    /* ^D: end of file on an empty line, otherwise the partial line. */
    feed(&t, "\x04");
    CHECK(tty_read(&t, buf, sizeof(buf)) == 0);
    feed(&t, "par\x04");
    CHECK(tty_read(&t, buf, sizeof(buf)) == 3 && memcmp(buf, "par", 3) == 0);
    feed(&t, "next\n");
    CHECK(tty_read(&t, buf, sizeof(buf)) == 5);
    /* A ^D record read with a buffer that fits the text exactly. */
    feed(&t, "abc\x04");
    CHECK(tty_read(&t, buf, 3) == 3 && memcmp(buf, "abc", 3) == 0);
    CHECK(t.lines == 0);   /* the terminator went with it */

    /* Control bytes other than the editing keys are dropped (^C too). */
    feed(&t, "x\x03y\x1b\x01z\n");
    CHECK(tty_read(&t, buf, sizeof(buf)) == 4 && memcmp(buf, "xyz\n", 4) == 0);

    /* Line length: TTY_LINE_MAX - 1 characters plus the newline. */
    static char big[1200];
    memset(big, 'a', sizeof(big));
    tty_input(&t, (const uint8_t *)big, 1100);
    feed(&t, "\n");
    struct tty_stats st;
    tty_get_stats(&t, &st);
    CHECK(st.dropped_bytes == 1100 - (TTY_LINE_MAX - 1));
    static char line[TTY_LINE_MAX + 8];
    CHECK(tty_read(&t, line, sizeof(line)) == (int64_t)TTY_LINE_MAX);
    CHECK(line[TTY_LINE_MAX - 1] == '\n' && line[0] == 'a');

    /* Ring limit: lines beyond TTY_INPUT_MAX are dropped, older ones kept. */
    for (unsigned i = 0; i < 100; i++) {
        tty_input(&t, (const uint8_t *)big, 99);
        feed(&t, "\n");
    }
    tty_get_stats(&t, &st);
    CHECK(st.dropped_lines > 0 && t.lines == TTY_INPUT_MAX / 100);
    for (unsigned i = 0; i < TTY_INPUT_MAX / 100; i++)
        CHECK(tty_read(&t, line, sizeof(line)) == 100);
    CHECK(t.used == 0 && t.lines == 0);

    /* A blocked reader wakes when a line completes. */
    struct reader r = { .tty = &t };
    struct thread *th = thread_create(reader_thread, &r, "tty-reader", 32);
    CHECK(th != NULL);
    thread_sleep_ms(20);
    CHECK(!r.done);
    feed(&t, "wake\n");
    for (unsigned i = 0; i < 100 && !r.done; i++)
        thread_sleep_ms(10);
    CHECK(r.done && r.got == 5 && memcmp(r.buf, "wake\n", 5) == 0);
    thread_join(th);

    /*
     * A blocked reader is released by a mode change. It is waiting for a
     * byte under `VMIN` 1; `VMIN` 0 withdraws that promise -- the read
     * must answer with nothing rather than sleep on a contract the
     * terminal no longer offers.
     */
    struct cosmo_termios tio;
    tty_get_termios(&t, &tio);
    tio.modes &= ~(uint32_t)COSMO_TTY_ICANON;
    tio.vmin = 1;
    tty_set_termios(&t, &tio);
    struct reader r2 = { .tty = &t };
    struct thread *th2 = thread_create(reader_thread, &r2, "tty-vmin", 32);
    CHECK(th2 != NULL);
    thread_sleep_ms(20);
    CHECK(!r2.done);   /* VMIN 1 with an empty ring: it waits */
    tio.vmin = 0;
    tty_set_termios(&t, &tio);
    for (unsigned i = 0; i < 100 && !r2.done; i++)
        thread_sleep_ms(10);
    CHECK(r2.done && r2.got == 0);
    thread_join(th2);
    tio.modes |= COSMO_TTY_ICANON;   /* and back, for what follows */
    tio.vmin = 1;
    tty_set_termios(&t, &tio);

    /* Zero-length read never blocks. */
    CHECK(tty_read(&t, buf, 0) == 0);
    tty_get_stats(&t, &st);
    CHECK(st.eofs == 1 && st.lines_in > 0);
    return true;
}

/* --- tty-devready: the terminal's files report readiness (the
 * device-readiness unit). Through the /dev/console file: what the console
 * object answers, per open non-blocking, and /dev/tty with no session. --- */

#include <kernel/object.h>
#include <kernel/poll.h>
#include <kernel/timer.h>
#include <kernel/vfs.h>

struct devready_feeder {
    struct tty *tty;
    unsigned delay_ms;
};

static void devready_feeder_main(void *arg)
{
    struct devready_feeder *fd = arg;
    thread_sleep_ms(fd->delay_ms);
    feed(fd->tty, "devready line\n");
    thread_exit(0);
}

#define DCHECK(cond)                                                                         \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            *reason = "tty-devready: " #cond;                                                \
            ok = false;                                                                      \
            goto out;                                                                        \
        }                                                                                    \
    } while (0)

bool selftest_tty_devready(const char **reason)
{
    bool ok = true;
    struct file *a = NULL, *b = NULL, *t = NULL;
    struct thread *th = NULL;
    struct tty *con = tty_console();
    char buf[64];

    /* Two opens of /dev/console: readiness is the terminal's, the mode is each open's. */
    DCHECK(vfs_open(NULL, "/dev/console", COSMO_O_RDWR, 0, &a) == 0);
    DCHECK(vfs_open(NULL, "/dev/console", COSMO_O_RDWR, 0, &b) == 0);
    DCHECK(kobject_poll_wq(&a->obj, COSMO_IO_READABLE) == &con->readers);
    DCHECK(kobject_poll_wq(&a->obj, COSMO_IO_WRITABLE) == NULL);       /* never blocks a writer */
    /* Nothing typed: not readable, and the answer agrees with the console object's. */
    unsigned r0 = kobject_ready(&a->obj);
    DCHECK((r0 & COSMO_IO_WRITABLE) != 0);
    DCHECK((r0 & COSMO_IO_READABLE) == (tty_read_ready(con) ? COSMO_IO_READABLE : 0));
    if (r0 & COSMO_IO_READABLE) {
        /* Something was already queued (a harness that types early): drain it so the
         * waits below start from an empty terminal. */
        DCHECK(kobject_set_nonblock(&a->obj, 1) == 0);
        while (file_read(a, buf, sizeof(buf)) > 0)
            ;
        DCHECK(kobject_set_nonblock(&a->obj, 0) == 1);
        DCHECK((kobject_ready(&a->obj) & COSMO_IO_READABLE) == 0);
    }
    /* Per open: switch one, the other keeps waiting. */
    DCHECK(kobject_set_nonblock(&a->obj, -1) == 0 && kobject_set_nonblock(&b->obj, -1) == 0);
    DCHECK(kobject_set_nonblock(&a->obj, 1) == 0);
    DCHECK(kobject_set_nonblock(&a->obj, -1) == 1 && kobject_set_nonblock(&b->obj, -1) == 0);
    DCHECK(file_read(a, buf, sizeof(buf)) == -EAGAIN);                  /* non-blocking, nothing typed */
    /* A poll with nothing typed times out; one with a line on the way wakes. */
    struct io_pollfd pf = { .obj = &a->obj, .events = COSMO_IO_READABLE };
    DCHECK(io_poll(&pf, 1, 20 * 1000000ull) == 0);
    struct devready_feeder fdr = { .tty = con, .delay_ms = 30 };
    th = thread_create(devready_feeder_main, &fdr, "devready-feed", 32);
    DCHECK(th != NULL);
    uint64_t t0 = clock_now_ns();
    DCHECK(io_poll(&pf, 1, 2000 * 1000000ull) == 1);
    DCHECK((pf.revents & COSMO_IO_READABLE) != 0);
    DCHECK(clock_since_ns(t0) < 1500 * 1000000ull);
    thread_join(th);
    th = NULL;
    DCHECK(kobject_ready(&b->obj) & COSMO_IO_READABLE);                /* the other open sees the same terminal */
    int64_t n = file_read(b, buf, sizeof(buf));                          /* blocking open: takes the line */
    DCHECK(n == 14 && memcmp(buf, "devready line\n", 14) == 0);
    DCHECK(file_read(a, buf, sizeof(buf)) == -EAGAIN);                  /* the non-blocking one finds it gone */
    /* The console object itself is still not switchable (it is everybody's). */
    DCHECK(kobject_set_nonblock(console_object(), -1) == -EOPNOTSUPP);
    /* /dev/tty from a kernel thread: no session, no terminal -- ERROR, no queue, -ENXIO. */
    DCHECK(vfs_open(NULL, "/dev/tty", COSMO_O_RDWR, 0, &t) == 0);
    DCHECK(kobject_ready(&t->obj) == COSMO_IO_ERROR);
    DCHECK(kobject_poll_wq(&t->obj, COSMO_IO_READABLE) == NULL);
    DCHECK(file_read(t, buf, sizeof(buf)) == -ENXIO);
    kinfo("selftest: tty-devready: /dev/console reports the terminal's readiness, its mode is per open, /dev/tty without a session is an error");
out:
    if (th)
        thread_join(th);
    if (a) file_put(a);
    if (b) file_put(b);
    if (t) file_put(t);
    return ok;
}
