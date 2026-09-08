/*
 * hidtest.c - The keyboard, end to end: keys typed outside the machine
 * arrive as a line on the console tty
 * (docs/drivers/usb/testing.md, "The keyboard").
 *
 * The harness types over QEMU's monitor protocol (tests/boot/keytest.py),
 * which is as close to a person at a keyboard as a test can get: the key
 * events go into the emulated device, the device reports them on its
 * interrupt endpoint, the driver translates and calls tty_input, and this
 * test reads the line back out of the tty the shell reads.
 *
 * The guest cannot know by itself whether anything will type, so the
 * harness says so through fw_cfg (opt/cosmo/keytest, as the network test
 * does). Without it the test skips; with it, silence is a failure.
 */

#include <kernel/errno.h>
#include <kernel/fwcfg.h>
#include <kernel/log.h>
#include <kernel/printf.h>
#include <kernel/selftest.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/tty.h>

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            *reason = "hid: " #cond;                                                         \
            return false;                                                                    \
        }                                                                                    \
    } while (0)

/* What tests/boot/keytest.py types, and what the tty must hand back. */
#define KEYTEST_LINE "cosmo Types 42!"
/* The second line is typed with the keys overlapping -- x down, y down,
 * x up, y up -- which is the case a boot report cannot describe as
 * events: with both keys held the report says "x and y" twice over. A
 * driver that types what the report holds instead of what changed in it
 * sends "xxyy". */
#define KEYTEST_ROLLOVER "xy"
/* The typing began at the start of the self-test run, so on any machine
 * the lines are here by now; this is what a failure costs to report.
 * Bounded by the clock and not by a count of sleeps: on a loaded build
 * runner a `thread_sleep_ms(1)` is worth several milliseconds, and the
 * first version of this waited 20 s where it meant to wait 5. */
#define KEYTEST_WAIT_NS (5ull * 1000 * 1000 * 1000)

static int64_t read_a_line(char *buf, size_t len)
{
    uint64_t deadline = clock_now_ns() + KEYTEST_WAIT_NS;
    for (;;) {
        if (tty_has_line(tty_console())) {
            int64_t got = tty_read(tty_console(), buf, len - 1);
            if (got > 0) {
                buf[got] = '\0';
                if (buf[got - 1] == '\n')
                    buf[got - 1] = '\0';
            }
            return got;
        }
        if (clock_now_ns() >= deadline)
            return -1;
        thread_sleep_ms(1);
    }
}

static bool g_armed;
static struct tty_stats g_before;
static unsigned g_flags;

/*
 * Ask for the keys, and return. The harness types over a socket into an
 * emulated device in a machine emulated on a loaded build runner, so how
 * long that takes is not this machine's business and must not be one
 * test's duration: the check runs at the end of the self-tests, by which
 * time the lines have long arrived (`hid-keyboard`, below).
 */
bool selftest_hid_arm(const char **reason)
{
    (void)reason;
    char want[8];

    /* The driver lives in a module and the kernel does not call into
     * modules, so what says a keyboard is there is the harness: it sets
     * this only when it has attached one and will type at it. */
    if (!fwcfg_get_string("keytest", want, sizeof(want))) {
        kinfo("selftest: hid-keyboard: nothing will type at this machine; skipped");
        return true;
    }
    tty_get_stats(tty_console(), &g_before);
    /* Echo off while the harness types: the keys arrive over the whole
     * self-test run, and echoed characters in the middle of a line the
     * harness parses are a flaky boot, not a test. */
    g_flags = tty_set_flags(tty_console(), 0);
    g_armed = true;
    /* The harness waits for this line before it starts typing, so that
     * the keys cannot arrive while an earlier test still owns the tty. */
    kprintf("HID-KEYTEST-READY\n");
    return true;
}

bool selftest_hid_keyboard(const char **reason)
{
    if (!g_armed)
        return true;   /* nothing typed at this machine; hid-arm said so */
    struct tty_stats before = g_before;
    tty_set_flags(tty_console(), g_flags);   /* the shell wants its echo back */

    char line[64];
    int64_t got = read_a_line(line, sizeof(line));
    if (got <= 0) {
        struct tty_stats now;
        tty_get_stats(tty_console(), &now);
        kerror("selftest: hid-keyboard: nothing arrived (the tty took %llu bytes and %llu lines while waiting)",
               (unsigned long long)(now.rx_bytes - before.rx_bytes),
               (unsigned long long)(now.lines_in - before.lines_in));
    }
    CHECK(got > 0);
    if (strcmp(line, KEYTEST_LINE) != 0)
        kerror("selftest: hid-keyboard: read \"%s\", expected \"%s\"", line, KEYTEST_LINE);
    CHECK(strcmp(line, KEYTEST_LINE) == 0);

    /* The overlapping pair: exactly two characters, in order, however
     * many reports the two keys' four transitions produced. */
    char rollover[64];
    int64_t got2 = read_a_line(rollover, sizeof(rollover));
    if (got2 <= 0 || strcmp(rollover, KEYTEST_ROLLOVER) != 0) {
        struct tty_stats now;
        tty_get_stats(tty_console(), &now);
        kerror("selftest: hid-keyboard: keys held together read \"%s\" (%lld bytes), expected \"%s\"; "
               "the tty took %llu bytes and %llu lines in all",
               got2 > 0 ? rollover : "", (long long)got2, KEYTEST_ROLLOVER,
               (unsigned long long)(now.rx_bytes - before.rx_bytes),
               (unsigned long long)(now.lines_in - before.lines_in));
    }
    CHECK(got2 > 0 && strcmp(rollover, KEYTEST_ROLLOVER) == 0);

    /* Every character of the line, plus its newline, came in through
     * tty_input while this test waited -- so the bytes are the keyboard's
     * and not something left in the ring by an earlier test. */
    struct tty_stats after;
    tty_get_stats(tty_console(), &after);
    CHECK(after.rx_bytes - before.rx_bytes >= strlen(KEYTEST_LINE) + strlen(KEYTEST_ROLLOVER) + 2);
    CHECK(after.lines_in >= before.lines_in + 2);
    CHECK(after.dropped_lines == before.dropped_lines && after.dropped_bytes == before.dropped_bytes);
    kinfo("selftest: hid-keyboard: \"%s\" and \"%s\" (typed with the keys overlapping) reached the tty "
          "(%llu bytes, %llu lines)",
          KEYTEST_LINE, KEYTEST_ROLLOVER, (unsigned long long)(after.rx_bytes - before.rx_bytes),
          (unsigned long long)(after.lines_in - before.lines_in));
    return true;
}
