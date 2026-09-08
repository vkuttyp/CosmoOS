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
#include <kernel/log.h>
#include <kernel/selftest.h>
#include <kernel/string.h>
#include <kernel/timer.h>

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
    const uint8_t *glyph = font8x8[font_index(ch)];
    for (uint32_t gy = 0; gy < FONT_HEIGHT; gy++) {
        for (uint32_t gx = 0; gx < FONT_WIDTH; gx++) {
            uint32_t want = (glyph[gy] & (0x80u >> gx)) ? g->fg : g->bg;
            uint32_t got = read_pixel(g, col * FONT_WIDTH + gx, row * FONT_HEIGHT + gy);
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
        uint64_t dt = clock_now_ns() - t0;
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
