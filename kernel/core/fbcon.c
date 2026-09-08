/*
 * fbcon.c - A text console on the framebuffer the firmware already lit.
 *
 * The loader reports the Graphics Output Protocol's current mode (boot
 * protocol v6); this file maps that memory once the VMM can, draws
 * characters into it with the generated 8x8 face, and registers itself as
 * a second console sink beside the serial port. It never sets a mode,
 * allocates a scanout, or talks to a display device: the pixels it writes
 * are the ones the firmware is already scanning out. Choosing modes is a
 * display driver's job (constitution section 60, "GPU later").
 *
 * Concurrency and context: console.c serialises sink writes under its own
 * IRQ-safe lock and drops that lock for good in panic mode, so this sink
 * keeps no lock of its own -- exactly like the serial sinks. Everything
 * here is bounded, allocation-free and sleep-free, because it runs from
 * interrupt and panic context.
 *
 * See docs/kernel/diagnostics/design.md, "The framebuffer console".
 */

#include <kernel/bootinfo.h>
#include <kernel/console.h>
#include <kernel/fbcon.h>
#include <kernel/font.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/string.h>
#include <kernel/vmm.h>

struct fbcon {
    volatile uint8_t *base;      /* mapped framebuffer, first visible pixel */
    uint32_t pitch;              /* bytes per row */
    uint32_t bytes_pp;
    uint32_t width, height;      /* pixels */
    uint32_t cols, rows;         /* cells */
    uint32_t cx, cy;             /* cursor, in cells */
    uint32_t fg, bg;             /* packed pixel values */
    char *text;                  /* cols * rows cells, the screen as characters */
    uint32_t scroll_rows;        /* cell rows moved by one scroll */
    bool ready;
    struct console_sink sink;
    struct fbcon_stats stats;
};

static struct fbcon g_fbcon;

/* An eight-bit intensity in a channel of `bits` bits. Deeper channels
 * exist (ten bits a colour is a real mode) and the validator allows them,
 * so this scales both ways rather than shifting by a negative amount. */
static uint32_t channel(unsigned value8, uint8_t bits)
{
    if (bits >= 8)
        return (uint32_t)value8 << (bits - 8);
    return (uint32_t)value8 >> (8 - bits);
}

static uint32_t pack(const struct bootinfo_framebuffer *fb, unsigned r, unsigned g, unsigned b)
{
    return (channel(r, fb->red_bits) << fb->red_shift) |
           (channel(g, fb->green_bits) << fb->green_shift) |
           (channel(b, fb->blue_bits) << fb->blue_shift);
}

static inline void put_pixel(struct fbcon *c, uint32_t x, uint32_t y, uint32_t value)
{
    volatile uint8_t *p = c->base + (size_t)y * c->pitch + (size_t)x * c->bytes_pp;
    switch (c->bytes_pp) {
    case 4:
        *(volatile uint32_t *)p = value;
        break;
    case 2:
        *(volatile uint16_t *)p = (uint16_t)value;
        break;
    case 1:
        *p = (uint8_t)value;
        break;
    default:   /* 3 */
        p[0] = (uint8_t)value;
        p[1] = (uint8_t)(value >> 8);
        p[2] = (uint8_t)(value >> 16);
        break;
    }
}

/* Fill whole cell rows [first, first + count) with the background. */
static void clear_rows(struct fbcon *c, uint32_t first, uint32_t count)
{
    uint32_t y0 = first * FONT_HEIGHT;
    uint32_t y1 = y0 + count * FONT_HEIGHT;
    if (y1 > c->height)
        y1 = c->height;
    for (uint32_t y = y0; y < y1; y++) {
        for (uint32_t x = 0; x < c->width; x++)
            put_pixel(c, x, y, c->bg);
    }
}

static void draw_glyph(struct fbcon *c, uint32_t col, uint32_t row, unsigned char ch)
{
    const uint8_t *glyph = font8x8[font_index(ch)];
    uint32_t x0 = col * FONT_WIDTH;
    uint32_t y0 = row * FONT_HEIGHT;

    for (uint32_t gy = 0; gy < FONT_HEIGHT; gy++) {
        uint8_t bits = glyph[gy];
        for (uint32_t gx = 0; gx < FONT_WIDTH; gx++) {
            uint32_t value = (bits & (0x80u >> gx)) ? c->fg : c->bg;
            put_pixel(c, x0 + gx, y0 + gy, value);
        }
    }
    c->stats.glyphs++;
}

/* Draw every cell from the text shadow. Writes only: reading an uncached
 * framebuffer costs several times what writing it does, so the screen is
 * never a source, only a destination. */
static void repaint(struct fbcon *c)
{
    for (uint32_t row = 0; row < c->rows; row++) {
        for (uint32_t col = 0; col < c->cols; col++)
            draw_glyph(c, col, row, (unsigned char)c->text[row * c->cols + col]);
    }
    c->stats.repaints++;
}

/*
 * Move the screen up by `scroll_rows` cell rows. The text moves in RAM and
 * the screen is repainted from it, so a scroll costs one screen of writes
 * and no reads.
 *
 * Several rows go at once because a scroll is the expensive operation and
 * its cost does not depend on how far it moves: one row at a time would
 * repaint the screen for every line printed, which on a boot of several
 * hundred lines is most of a gigabyte of bus traffic to draw frames
 * nobody sees. The cost per line falls with the chunk; what it buys is
 * paid for in blank space at the bottom, which the next lines fill.
 * (docs/kernel/diagnostics/testing.md, "Benchmarks".)
 */
static void scroll(struct fbcon *c)
{
    uint32_t n = c->scroll_rows;
    if (n > c->rows)
        n = c->rows;
    size_t keep = (size_t)(c->rows - n) * c->cols;

    memmove(c->text, c->text + (size_t)n * c->cols, keep);
    memset(c->text + keep, ' ', (size_t)n * c->cols);
    repaint(c);
    c->stats.scrolls++;
}

static void newline(struct fbcon *c)
{
    c->cx = 0;
    if (c->cy + 1 < c->rows) {
        c->cy++;
        return;
    }
    scroll(c);
    c->cy = c->rows - c->scroll_rows;
}

static void putc(struct fbcon *c, char ch)
{
    unsigned char b = (unsigned char)ch;

    switch (b) {
    case '\n':
        newline(c);
        return;
    case '\r':
        c->cx = 0;
        return;
    case '\b':
        if (c->cx > 0)
            c->cx--;
        return;   /* the tty erases by writing "\b \b"; the space does the clearing */
    case '\t':
        do {
            putc(c, ' ');
        } while ((c->cx % 8) != 0 && c->cx != 0);
        return;
    default:
        break;
    }
    if (b < 0x20 && b != 0)
        return;   /* other control bytes are not drawn */

    if (c->cx >= c->cols)
        newline(c);
    c->text[c->cy * c->cols + c->cx] = (char)b;
    draw_glyph(c, c->cx, c->cy, b);
    c->cx++;
}

static void fbcon_write(struct console_sink *sink, const char *s, size_t len)
{
    struct fbcon *c = container_of(sink, struct fbcon, sink);
    if (!c->ready)
        return;
    for (size_t i = 0; i < len; i++)
        putc(c, s[i]);
    c->stats.bytes += len;
}

/*
 * Show the newest screenful of the log ring, so the display starts with
 * what a reader would want rather than with the line that happened to be
 * printed when the mapping became possible. Older lines are not replayed:
 * scrolling them through an uncached framebuffer costs hundreds of
 * megabytes of bus traffic to draw frames nobody sees, and `dmesg` has
 * the whole ring either way.
 */
static void replay_log(struct fbcon *c)
{
    char *buf = kmalloc(KLOG_RING_SIZE, 0);
    if (buf == NULL)
        return;
    size_t n = klog_copy(buf, KLOG_RING_SIZE);

    /* Walk back over at most `rows` newlines. */
    size_t start = n;
    unsigned lines = 0;
    while (start > 0 && lines < c->rows) {
        start--;
        if (buf[start] == '\n' && ++lines == c->rows) {
            start++;
            break;
        }
    }
    fbcon_write(&c->sink, buf + start, n - start);
    kfree(buf);
}

/* True if [pa, pa+len) touches memory the page allocator may hand out. A
 * framebuffer there would be scribbled on by whoever got the page, or the
 * other way round: refuse it rather than corrupt one of the two. */
static bool overlaps_usable_ram(uint64_t pa, uint64_t len)
{
    uint32_t count = 0;
    const struct cosmoboot_mem_entry *map = bootinfo_mem_map(&count);
    for (uint32_t i = 0; i < count; i++) {
        if (map[i].type != COSMOBOOT_MEM_USABLE && map[i].type != COSMOBOOT_MEM_LOADER_RECLAIMABLE)
            continue;
        uint64_t lo = map[i].base, hi = map[i].base + map[i].length;
        if (pa < hi && lo < pa + len)
            return true;
    }
    return false;
}

void fbcon_init(void)
{
    const struct bootinfo_framebuffer *fb = bootinfo_framebuffer();
    if (fb == NULL)
        return;

    if (overlaps_usable_ram(fb->phys, fb->size)) {
        kwarn("fbcon: the framebuffer at phys 0x%llx overlaps free memory; not used",
              (unsigned long long)fb->phys);
        return;
    }

    uint64_t offset = fb->phys & (PAGE_SIZE - 1);
    uint64_t map_base = fb->phys - offset;
    size_t map_len = (size_t)ALIGN_UP(offset + fb->size, PAGE_SIZE);
    vaddr_t va = vm_map_phys((paddr_t)map_base, map_len, VM_PROT_RW, VM_CACHE_UC);
    if (va == 0) {
        kwarn("fbcon: cannot map %llu KiB of framebuffer at phys 0x%llx",
              (unsigned long long)(fb->size >> 10), (unsigned long long)fb->phys);
        return;
    }

    struct fbcon *c = &g_fbcon;
    c->base = (volatile uint8_t *)(va + (vaddr_t)offset);
    c->pitch = fb->pitch;
    c->bytes_pp = fb->bpp / 8;
    c->width = fb->width;
    c->height = fb->height;
    c->cols = fb->width / FONT_WIDTH;
    c->rows = fb->height / FONT_HEIGHT;
    c->cx = c->cy = 0;
    c->fg = pack(fb, 0xd0, 0xd0, 0xd0);
    c->bg = pack(fb, 0x00, 0x00, 0x00);
    c->sink.name = "fbcon";
    c->sink.write = fbcon_write;

    if (c->cols == 0 || c->rows == 0) {
        kwarn("fbcon: %ux%u is smaller than one character cell", c->width, c->height);
        vm_unmap_phys(va);
        return;
    }

    /* The screen as characters. Allocated once, here: the sink itself
     * runs in interrupt and panic context and never allocates. */
    c->text = kmalloc((size_t)c->cols * c->rows, 0);
    if (c->text == NULL) {
        kwarn("fbcon: cannot allocate a %ux%u text buffer", c->cols, c->rows);
        vm_unmap_phys(va);
        return;
    }
    memset(c->text, ' ', (size_t)c->cols * c->rows);
    c->scroll_rows = c->rows / 8 ? c->rows / 8 : 1;

    c->ready = true;
    clear_rows(c, 0, c->rows);
    replay_log(c);
    console_register(&c->sink);
    kinfo("fbcon: %ux%u cells of %ux%u pixels at %p, scrolling %u rows at a time", c->cols, c->rows,
          FONT_WIDTH, FONT_HEIGHT, (void *)(uintptr_t)c->base, c->scroll_rows);
}

bool fbcon_present(void)
{
    return g_fbcon.ready;
}

void fbcon_get_stats(struct fbcon_stats *out)
{
    *out = g_fbcon.stats;
}

bool fbcon_geometry(struct fbcon_geometry *out)
{
    if (!g_fbcon.ready || out == NULL)
        return false;
    out->base = (volatile void *)g_fbcon.base;
    out->pitch = g_fbcon.pitch;
    out->bytes_pp = g_fbcon.bytes_pp;
    out->width = g_fbcon.width;
    out->height = g_fbcon.height;
    out->cols = g_fbcon.cols;
    out->rows = g_fbcon.rows;
    out->fg = g_fbcon.fg;
    out->bg = g_fbcon.bg;
    out->scroll_rows = g_fbcon.scroll_rows;
    return true;
}

void fbcon_cursor(uint32_t *col, uint32_t *row)
{
    if (col)
        *col = g_fbcon.cx;
    if (row)
        *row = g_fbcon.cy;
}
