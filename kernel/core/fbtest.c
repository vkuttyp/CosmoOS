/*
 * fbtest.c - Self-tests of the framebuffer console.
 *
 * A display is testable without a screen or a screenshot: the console
 * writes through the ordinary path and the test reads the pixels back
 * through the same mapping and compares them against the font table. Both
 * colours are checked, so a wrong pixel format fails here rather than
 * drawing something illegible that a log marker would still match.
 *
 * The screen is shared with everything else that prints, so a check that
 * loses a race with another CPU's log line is retried rather than failed;
 * three attempts is far more than the noise a quiet self-test run makes.
 *
 * docs/kernel/diagnostics/testing.md, "The framebuffer console".
 */

#include <kernel/console.h>
#include <kernel/fbcon.h>
#include <kernel/font.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/printf.h>
#include <kernel/selftest.h>
#include <kernel/string.h>
#include <kernel/timer.h>
#include <kernel/tty.h>

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            *reason = "fbcon: " #cond;                                                       \
            return false;                                                                    \
        }                                                                                    \
    } while (0)

static uint32_t read_pixel(const struct fbcon_geometry *g, uint32_t x, uint32_t y)
{
    const volatile uint8_t *p = (const volatile uint8_t *)g->base + (size_t)y * g->pitch +
                                (size_t)x * g->bytes_pp;
    switch (g->bytes_pp) {
    case 4:
        return *(const volatile uint32_t *)p;
    case 2:
        return *(const volatile uint16_t *)p;
    case 1:
        return *p;
    default:
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    }
}

/* Every pixel of one cell against the glyph the font says it should be. */
static bool cell_is(const struct fbcon_geometry *g, uint32_t col, uint32_t row, unsigned char ch)
{
    const uint8_t *glyph = font8x16[font_index(ch)];
    uint32_t s = g->scale, w = FONT_WIDTH * s, h = FONT_HEIGHT * s;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t want = (glyph[y / s] & (0x80u >> (x / s))) ? g->fg : g->bg;
            uint32_t got = read_pixel(g, col * w + x, row * h + y);
            if (got != want)
                return false;
        }
    }
    return true;
}

static bool cells_are(const struct fbcon_geometry *g, uint32_t col, uint32_t row, const char *s)
{
    for (uint32_t i = 0; s[i] != '\0'; i++) {
        if (!cell_is(g, col + i, row, (unsigned char)s[i]))
            return false;
    }
    return true;
}

/*
 * Write `s` at the start of a fresh line and report where it landed. The
 * leading newline puts the cursor at column 0 so the caller knows the
 * column; the row is read back because a scroll may have moved it.
 */
static void write_line(const char *s, uint32_t *row)
{
    console_puts("\n");
    console_puts(s);
    uint32_t col = 0;
    fbcon_cursor(&col, row);
}

bool selftest_fb_console(const char **reason)
{
    if (!fbcon_present()) {
        kinfo("selftest: fb-console: no framebuffer on this machine; skipped");
        return true;
    }
    struct fbcon_geometry geom;
    CHECK(fbcon_geometry(&geom));
    /* The pixels are read through `base` without the console lock. The
     * boot framebuffer's mapping is never freed, even when a surface is
     * attached meanwhile; a display driver's surface can be replaced and
     * freed by a display change at any time (Codex, PR #387). `surface`
     * comes from the same hold as `base` (Qodo). */
    if (geom.surface) {
        kinfo("selftest: fb-console: a display driver's surface holds the console; skipped");
        return true;
    }
    const struct fbcon_geometry *g = &geom;
    CHECK(g->cols >= 40 && g->rows >= 8);
    CHECK(g->fg != g->bg);

    static const char marker[] = "fbcon selftest 0123456789 !@#$%^&*()";
    const uint32_t len = (uint32_t)(sizeof(marker) - 1);
    CHECK(len < g->cols);

    /* Drawn: every pixel of every cell is the glyph the font holds. */
    bool drawn = false;
    uint32_t row = 0;
    for (unsigned attempt = 0; attempt < 3 && !drawn; attempt++) {
        write_line(marker, &row);
        drawn = cells_are(g, 0, row, marker);
    }
    CHECK(drawn);

    /* Erased: the tty echoes a backspace as "\b \b", so the cell the
     * cursor came back over reads as a space afterwards. */
    bool erased = false;
    for (unsigned attempt = 0; attempt < 3 && !erased; attempt++) {
        write_line("Z", &row);
        uint32_t col = 0;
        fbcon_cursor(&col, &row);
        if (col != 1 || !cell_is(g, 0, row, 'Z'))
            continue;
        console_puts("\b \b");
        fbcon_cursor(&col, &row);
        erased = col == 0 && cell_is(g, 0, row, ' ');
    }
    CHECK(erased);

    /* Not drawable: a byte outside the font is the replacement box, and
     * the box is not the space (a silently blank cell would pass a test
     * that only asked "did something appear"). */
    bool boxed = false;
    for (unsigned attempt = 0; attempt < 3 && !boxed; attempt++) {
        char odd[2] = { (char)0xa9, '\0' };   /* not in 0x20..0x7e */
        write_line(odd, &row);
        boxed = cell_is(g, 0, row, (unsigned char)0xa9) && !cell_is(g, 0, row, ' ');
    }
    CHECK(boxed);

    /* Wrapped: a line longer than the screen continues on the next row
     * without losing a character. */
    bool wrapped = false;
    for (unsigned attempt = 0; attempt < 3 && !wrapped; attempt++) {
        console_puts("\n");
        for (uint32_t i = 0; i < g->cols; i++)
            console_puts("w");
        console_puts("W");
        uint32_t col = 0, wrow = 0;
        fbcon_cursor(&col, &wrow);
        wrapped = col == 1 && cell_is(g, 0, wrow, 'W') && wrow > 0 &&
                  cell_is(g, g->cols - 1, wrow - 1, 'w');
    }
    CHECK(wrapped);

    /* Scrolled: a marker written near the bottom moves up by exactly
     * scroll_rows when the screen fills, and the text below it is blank.
     * The scroll is the operation that repaints everything, so this also
     * says the repaint draws what the shadow holds. */
    struct fbcon_stats before, after;
    bool scrolled = false;
    for (unsigned attempt = 0; attempt < 3 && !scrolled; attempt++) {
        uint32_t start = 0;
        write_line(marker, &start);
        /* Count scrolls from here, not from before the marker: writing
         * the marker can itself scroll when the cursor was at the
         * bottom, and then this would be counting two. */
        fbcon_get_stats(&before);
        if (start < g->scroll_rows || start + 1 >= g->rows)
            continue;   /* too near an edge to say where the marker moved */
        /* Fill to the bottom: one newline per remaining row forces one
         * scroll and no more. */
        for (uint32_t i = start; i + 1 < g->rows; i++)
            console_puts("\n");
        console_puts("\n");   /* the one that scrolls */
        fbcon_get_stats(&after);
        if (after.scrolls != before.scrolls + 1)
            continue;
        scrolled = cells_are(g, 0, start - g->scroll_rows, marker);
    }
    CHECK(scrolled);

    kinfo("selftest: fb-console: %ux%u cells, %llu glyphs and %llu scrolls so far", g->cols, g->rows,
          (unsigned long long)after.glyphs, (unsigned long long)after.scrolls);
    return true;
}

/*
 * What the console costs, which decides how it scrolls (constitution
 * section 21: no complexity without a measured benefit). Reports only.
 *
 * Both figures are measured through the real path -- console_puts, under
 * the console's own IRQ-safe lock -- because that is what a log line
 * costs the machine, and the scroll's share of it is what the chunk size
 * is chosen against.
 */
bool selftest_fb_bench(const char **reason)
{
    (void)reason;
    if (!fbcon_present()) {
        kinfo("selftest: fb-bench: no framebuffer on this machine; skipped");
        return true;
    }
    struct fbcon_geometry geom;
    if (!fbcon_geometry(&geom))
        return true;
    if (geom.surface) {   /* as in fb-console: base may be freed under the reads */
        kinfo("selftest: fb-bench: a display driver's surface holds the console; skipped");
        return true;
    }
    const struct fbcon_geometry *g = &geom;
    char line[81];
    unsigned width = g->cols < 80 ? g->cols : 80;
    memset(line, 'M', width - 1);
    line[width - 1] = '\n';
    line[width] = '\0';

    uint64_t plain_ns = 0, scroll_ns = 0;
    unsigned plain = 0, scrolls = 0;
    struct fbcon_stats a, b;

    for (unsigned i = 0; i < 3 * g->rows; i++) {
        fbcon_get_stats(&a);
        uint64_t t0 = clock_now_ns();
        console_puts(line);
        uint64_t dt = clock_since_ns(t0);
        fbcon_get_stats(&b);
        if (b.scrolls != a.scrolls) {
            scroll_ns += dt;
            scrolls++;
        } else {
            plain_ns += dt;
            plain++;
        }
    }
    if (plain == 0 || scrolls == 0) {
        kinfo("selftest: fb-bench: %u plain lines, %u scrolls; nothing to divide", plain, scrolls);
        return true;
    }
    uint64_t per_line = plain_ns / plain;
    uint64_t per_scroll = scroll_ns / scrolls;
    kinfo("selftest: fb-bench: %u columns: a line costs %llu us (%llu ns a glyph), a scroll of %u rows "
          "%llu us; amortised %llu us a line",
          width - 1, (unsigned long long)(per_line / 1000),
          (unsigned long long)(per_line / (width - 1)), g->scroll_rows,
          (unsigned long long)(per_scroll / 1000),
          (unsigned long long)((per_line + per_scroll / g->scroll_rows) / 1000));
    return true;
}

/*
 * A display driver's switch to a surface of another size (fbcon_attach,
 * the virtio-gpu unit's resize): the screen goes with it, and the console
 * terminal's size is the new grid. Two surfaces in memory, 60x17 cells
 * then 40x12, over whatever fbcon had (the debug boots' firmware
 * framebuffer, or none). Lines written through the sinks (console_puts,
 * the path tty.c's output takes, never the log ring) must be on the
 * second surface, the cursor after them: fbcon_attach's own log line
 * continues the unfinished one at column 15 (a scroll may since have
 * moved the rows, so they are looked for). A size set by
 * TIOCSWINSZ in between must give way to 40x12 (tty_console_resized,
 * which also raises SIGWINCH); one set after the switch must survive that
 * switch's notice arriving late. Detaching gives back the console there was,
 * at its size. Not run over a display driver's surface: detaching would
 * leave the device's screen.
 */
#define SW_ATTEMPTS 3u
bool selftest_fb_switch(const char **reason)
{
    struct fbcon_geometry g0 = { 0 };
    bool present0 = fbcon_geometry(&g0);
    if (g0.surface) {
        kinfo("selftest: fb-switch: a display driver holds the console; not replaced");
        return true;
    }
    static const struct fbcon_surface proto = {
        .bpp = 32, .red_shift = 16, .red_bits = 8, .green_shift = 8, .green_bits = 8,
        .blue_shift = 0, .blue_bits = 8,
    };
    struct fbcon_surface a = proto, b = proto;
    a.width = 480, a.height = 272, a.pitch = 480 * 4;
    b.width = 320, b.height = 200, b.pitch = 320 * 4;
    a.base = kmalloc((size_t)a.pitch * a.height, 0);
    b.base = kmalloc((size_t)b.pitch * b.height, 0);
    struct tty *con = tty_console();
    struct cosmo_ttysize sz;
    bool attached = false, kept = false, sized = false, late = false;
    uint32_t row = 0;
    if (a.base == NULL || b.base == NULL) {
        *reason = "fbcon: no memory for the surfaces";
        goto out;
    }
    for (unsigned attempt = 0; attempt < SW_ATTEMPTS && !(kept && sized); attempt++) {
        if (fbcon_attach(&a) != 0) {
            *reason = "fbcon: fbcon_attach(60x17) failed";
            goto out;
        }
        attached = true;
        console_puts("\nfb-switch kept\nfb-switch typed");
        tty_set_size(con, &(struct cosmo_ttysize){ .cols = 7, .rows = 3 });
        if (fbcon_attach(&b) != 0) {
            *reason = "fbcon: fbcon_attach(40x12) failed";
            goto out;
        }
        struct fbcon_geometry g;
        if (!fbcon_geometry(&g) || g.cols != 40 || g.rows != 12) {
            *reason = "fbcon: the second surface is not 40x12 cells";
            goto out;
        }
        /* Another CPU's line between ours and the switch's: tried again. */
        kept = false;
        for (row = 1; row < g.rows && !kept; row++)
            kept = cells_are(&g, 0, row, "fb-switch typed[ INFO] fbcon: ") &&
                   cells_are(&g, 0, row - 1, "fb-switch kept");
        tty_get_size(con, &sz);
        sized = sz.cols == 40 && sz.rows == 12;
        console_puts("\n");
    }
    if (!kept)
        *reason = "fbcon: the lines written before the switch are not on the new surface";
    else if (!sized)
        *reason = "fbcon: the console's size is not the new surface's 40x12";
    else {
        /* fbcon_attach's notice reaching the terminal after a program
         * set its size: the program's is newer and stays. */
        uint32_t gen = fbcon_switches();
        tty_set_size(con, &(struct cosmo_ttysize){ .cols = 9, .rows = 4 });
        tty_console_resized(gen);
        tty_get_size(con, &sz);
        late = sz.cols == 9 && sz.rows == 4;
        if (!late)
            *reason = "fbcon: a size set after the switch was dropped by its late notice";
    }
out:
    if (attached) {
        fbcon_detach(&b);   /* not attached: ignored */
        fbcon_detach(&a);
    }
    struct fbcon_geometry g1 = { 0 };
    bool present1 = fbcon_geometry(&g1);
    tty_get_size(con, &sz);
    kfree(a.base);
    kfree(b.base);
    if (!(kept && sized && late))
        return false;
    CHECK(present1 == present0 && !g1.surface);
    CHECK(g1.cols == g0.cols && g1.rows == g0.rows);
    CHECK(sz.cols == g0.cols && sz.rows == g0.rows);
    kinfo("selftest: fb-switch: %ux%u, then 60x17 to 40x12 cells: the screen moved (row %u), the cursor "
          "with it, the terminal's size followed (a newer one kept), and %ux%u is back",
          g0.cols, g0.rows, row - 1, g1.cols, g1.rows);
    return true;
}

/*
 * console-lines: a write of many lines in one call -- what `dmesg`'s one
 * write of the kernel log is -- reaches the sinks a line per hold of the
 * console lock, so interrupts are off for one line's drawing at a time.
 * One hold carried the whole write before: `dmesg` kept interrupts off on
 * its CPU for over a second while fbcon drew 32 KiB, and a TLB shootdown
 * waiting on that CPU panicked (docs/testing/flakes.md, 2026-10-21). Any
 * machine: the hold is the console's, whatever the sinks are.
 */
#define CL_LINES 24

bool selftest_console_lines(const char **reason)
{
    static char buf[CL_LINES * 80];
    size_t n = 0;
    for (int i = 0; i < CL_LINES; i++)
        n += (size_t)ksnprintf(buf + n, sizeof(buf) - n, "console-lines: line %2d of %d, a hold of its own\n", i + 1,
                               CL_LINES);
    (void)console_test_hold_max(true);
    timer_test_tick_gap_reset();
    uint64_t t0 = clock_now_ns();
    console_write(buf, n);
    uint64_t took = clock_since_ns(t0);
    size_t most = console_test_hold_max(false);
    unsigned cpu = 0;
    uint64_t gap = timer_test_tick_gap_max_ns(&cpu);
    kinfo("selftest: console-lines: %zu bytes in %d lines in %llu us; the most one hold carried was %zu bytes; the "
          "longest tick gap meanwhile %llu us (cpu %u)", n, CL_LINES, (unsigned long long)(took / 1000), most,
          (unsigned long long)(gap / 1000), cpu);
    /* Other CPUs' log lines are holds too, each one line: none carries
     * more than CONSOLE_HOLD_MAX, and this write's lines are far shorter. */
    CHECK(n > CONSOLE_HOLD_MAX);
    CHECK(most > 0 && most <= CONSOLE_HOLD_MAX);
    return true;
}

/*
 * console-sink-drop: a sink that gives up on part of a write (the virtio
 * console, when its device does not return a buffer within its spin) is
 * passed none of the rest of that write, while the other sinks get every
 * line: a write split into lines must not make a stalled device wait once
 * a line where it waited once a write. A sink of the test's own drops
 * every piece that begins with its mark and counts the pieces it is given.
 */
#define CSD_MARK "console-sink-drop: "
#define CSD_LINES 8

static unsigned g_csd_given, g_csd_after;   /* under the console lock */

static void csd_write(struct console_sink *sink, const char *s, size_t len)
{
    if (len < sizeof(CSD_MARK) - 1 || memcmp(s, CSD_MARK, sizeof(CSD_MARK) - 1) != 0)
        return;
    if (memcmp(s + sizeof(CSD_MARK) - 1, "after", 5) == 0) {
        g_csd_after++;
        return;
    }
    g_csd_given++;
    sink->dropped = true;
}

bool selftest_console_sink_drop(const char **reason)
{
    static struct console_sink sink = { .name = "selftest-drop", .write = csd_write };
    static char buf[CSD_LINES * 64];
    size_t n = 0;
    for (int i = 0; i < CSD_LINES; i++)
        n += (size_t)ksnprintf(buf + n, sizeof(buf) - n, CSD_MARK "line %d of %d, dropped by its sink\n", i + 1,
                               CSD_LINES);
    g_csd_given = g_csd_after = 0;
    console_register(&sink);
    console_write(buf, n);
    /* The next write is passed to it again. */
    console_puts(CSD_MARK "after, a write of its own\n");
    console_unregister(&sink);
    kinfo("selftest: console-sink-drop: %d lines in one write; the dropping sink was given %u of them, and %u of the "
          "next write", CSD_LINES, g_csd_given, g_csd_after);
    CHECK(g_csd_given == 1);
    CHECK(g_csd_after == 1);
    return true;
}
