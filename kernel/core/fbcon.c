/*
 * fbcon.c - A text console on the framebuffer the firmware already lit.
 *
 * The loader reports the Graphics Output Protocol's current mode (boot
 * protocol v6); this file maps that memory once the VMM can, draws
 * characters into it with the generated 8x16 face, each of its pixels an
 * s-by-s block for a large mode (fbcon_scale), and registers itself as
 * a second console sink beside the serial port. It never sets a mode (the
 * loader may have chosen one the display prefers, boot/uefi/display.c),
 * allocates a scanout, or talks to a display device: the pixels it writes
 * are the ones the firmware is already scanning out. Choosing modes is a
 * display driver's job: the virtio_gpu module, which moves the console
 * onto its own memory (fbcon_attach, below).
 *
 * Concurrency and context: console.c serialises sink writes under its own
 * IRQ-safe lock and drops that lock for good in panic mode, so this sink
 * keeps no lock of its own -- exactly like the serial sinks. Everything
 * here is bounded, allocation-free and sleep-free, because it runs from
 * interrupt and panic context.
 *
 * A display driver can move the console onto memory of its own
 * (fbcon_attach, the virtio-gpu unit): the device scans that memory out
 * only when told to, so the sink records the pixel rows it drew as one
 * damaged band and the driver's thread takes it (fbcon_take_damage) and
 * flushes. In panic mode no thread runs, and the sink calls the
 * surface's panic_flush itself after each write.
 *
 * See docs/kernel/diagnostics/design.md, "The framebuffer console".
 */

#include <kernel/bootinfo.h>
#include <kernel/console.h>
#include <kernel/errno.h>
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
    uint32_t scale;              /* each font pixel is scale x scale screen pixels */
    uint32_t cell_w, cell_h;     /* FONT_WIDTH * scale, FONT_HEIGHT * scale */
    uint32_t cols, rows;         /* cells */
    uint32_t cx, cy;             /* cursor, in cells */
    uint32_t fg, bg;             /* packed pixel values */
    char *text;                  /* cols * rows cells, the screen as characters */
    uint32_t scroll_rows;        /* cell rows moved by one scroll */
    bool ready;
    bool registered;             /* the sink is on the console's list */
    /* A driver's surface (fbcon_attach), or NULL for the boot framebuffer. */
    const struct fbcon_surface *surface;
    uint32_t dmg_y0, dmg_y1;     /* drawn and not yet taken: pixel rows [y0, y1), empty when equal */
    struct console_sink sink;
    struct fbcon_stats stats;
};

static struct fbcon g_fbcon;
/* The boot framebuffer's console, kept while a surface is attached so
 * fbcon_detach can go back to it (ready false: there was none). */
static struct fbcon g_boot_fbcon;

/* Pixel rows [y0, y1) were drawn: widen the damaged band. Only a surface
 * needs it; the boot framebuffer is scanned out as it is written. */
static void damage(struct fbcon *c, uint32_t y0, uint32_t y1)
{
    if (c->surface == NULL)
        return;
    if (y1 > c->height)
        y1 = c->height;
    if (y0 >= y1)
        return;
    if (c->dmg_y0 == c->dmg_y1) {
        c->dmg_y0 = y0;
        c->dmg_y1 = y1;
        return;
    }
    if (y0 < c->dmg_y0)
        c->dmg_y0 = y0;
    if (y1 > c->dmg_y1)
        c->dmg_y1 = y1;
}

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

/*
 * The drawing loops, each in a function of its own that cannot straddle a
 * page boundary: aligned to FBCON_DRAW_ALIGN and no larger than it, which
 * scripts/check-kernel-elf.sh checks on every link. Under QEMU's TCG a loop
 * whose instructions span two pages runs through an unchained translation
 * block every iteration: the CI-built crash kernel of the M2 branch had the
 * pixel loop across 0x...2000 and scrolled 11 times slower (118.8 ms a
 * scroll against 10.9 ms for the same code 64 bytes earlier), slowing every
 * test that logs much (docs/testing/flakes.md, syscall-fuzz, 2026-10-10).
 */
/* 512: the functions are 124 to 457 bytes (fbcon_draw_glyph the largest),
 * so an alignment of 256 kept them inside a page only where the linker
 * happened to put them -- the virtio-gpu branch's layout put
 * fbcon_draw_clear_rows (272 bytes) at 0x...2f00 on AArch64. */
#define FBCON_DRAW_ALIGN 512
#define FBCON_SCALE_MAX 4u
#define FBCON_DRAW __page_local(FBCON_DRAW_ALIGN)

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
static FBCON_DRAW void fbcon_draw_clear_rows(struct fbcon *c, uint32_t first, uint32_t count)
{
    uint32_t y0 = first * c->cell_h;
    uint32_t y1 = y0 + count * c->cell_h;
    if (y1 > c->height)
        y1 = c->height;
    for (uint32_t y = y0; y < y1; y++) {
        if (c->bytes_pp == 4) {
            volatile uint32_t *p = (volatile uint32_t *)(c->base + (size_t)y * c->pitch);
            for (uint32_t x = 0; x < c->width; x++)
                p[x] = c->bg;
            continue;
        }
        for (uint32_t x = 0; x < c->width; x++)
            put_pixel(c, x, y, c->bg);
    }
}

static FBCON_DRAW void fbcon_draw_glyph(struct fbcon *c, uint32_t col, uint32_t row, unsigned char ch)
{
    const uint8_t *glyph = font8x16[font_index(ch)];
    uint32_t s = c->scale;
    uint32_t x0 = col * c->cell_w;
    uint32_t y = row * c->cell_h;
    uint32_t line[FONT_WIDTH * FBCON_SCALE_MAX];   /* one font row, widened */

    /* Each font row is widened once into `line` and written `s` times: no
     * division per pixel, which under TCG doubled a repaint's cost. */
    /* Not unrolled: the function must fit its aligned block (above), and
     * clang unrolled these loops past a page in a release build. */
#pragma clang loop unroll(disable)
    for (uint32_t gy = 0; gy < FONT_HEIGHT; gy++) {
        uint8_t bits = glyph[gy];
        uint32_t n = 0;
#pragma clang loop unroll(disable)
        for (uint32_t gx = 0; gx < FONT_WIDTH; gx++) {
            uint32_t value = (bits & (0x80u >> gx)) ? c->fg : c->bg;
#pragma clang loop unroll(disable)
            for (uint32_t k = 0; k < s; k++)
                line[n++] = value;
        }
#pragma clang loop unroll(disable)
        for (uint32_t k = 0; k < s; k++, y++) {
            if (c->bytes_pp == 4) {
                /* The common format: the row as 32-bit stores, without
                 * put_pixel's switch per pixel. */
                volatile uint32_t *p = (volatile uint32_t *)(c->base + (size_t)y * c->pitch) + x0;
#pragma clang loop unroll(disable)
                for (uint32_t x = 0; x < n; x++)
                    p[x] = line[x];
                continue;
            }
#pragma clang loop unroll(disable)
            for (uint32_t x = 0; x < n; x++)
                put_pixel(c, x0 + x, y, line[x]);
        }
    }
    c->stats.glyphs++;
}

/* Draw every cell from the text shadow. Writes only: reading an uncached
 * framebuffer costs several times what writing it does, so the screen is
 * never a source, only a destination. */
static FBCON_DRAW void fbcon_draw_repaint(struct fbcon *c)
{
    for (uint32_t row = 0; row < c->rows; row++) {
        for (uint32_t col = 0; col < c->cols; col++)
            fbcon_draw_glyph(c, col, row, (unsigned char)c->text[row * c->cols + col]);
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
    fbcon_draw_repaint(c);
    damage(c, 0, c->rows * c->cell_h);
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
    fbcon_draw_glyph(c, c->cx, c->cy, b);
    damage(c, c->cy * c->cell_h, (c->cy + 1) * c->cell_h);
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
    /* No thread will flush during a panic: the surface does it now. */
    if (c->surface != NULL && c->surface->panic_flush != NULL && c->dmg_y0 != c->dmg_y1 &&
        console_in_panic_mode()) {
        uint32_t y0 = c->dmg_y0, y1 = c->dmg_y1;
        c->dmg_y0 = c->dmg_y1 = 0;
        c->surface->panic_flush(c->surface->arg, y0, y1);
    }
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

/*
 * How many screen pixels a font pixel is, each way. The face is 8x16, the
 * size of a PC or Linux text console, and is drawn one-to-one up to
 * 1920x1080 (Parallels Desktop's 1024x768: 128x48 cells, the owner's
 * choice on 2026-10-13 after trying 2x). A screen of twice that each way
 * (4K) and up draws it larger: min(width / 1920, height / 1080), 1 to 4.
 */
static uint32_t fbcon_scale(uint32_t width, uint32_t height)
{
    uint32_t s = width / 1920 < height / 1080 ? width / 1920 : height / 1080;
    return s < 1 ? 1 : s > FBCON_SCALE_MAX ? FBCON_SCALE_MAX : s;
}

/* Fill in the console over an already-mapped framebuffer at `base`, with
 * `text` (cols * rows cells) as its character buffer. False, with a
 * warning, when the mode cannot hold one cell. */
static bool fbcon_setup(struct fbcon *c, const struct bootinfo_framebuffer *fb, volatile uint8_t *base)
{
    c->base = base;
    c->pitch = fb->pitch;
    c->bytes_pp = fb->bpp / 8;
    c->width = fb->width;
    c->height = fb->height;
    c->scale = fbcon_scale(fb->width, fb->height);
    c->cell_w = FONT_WIDTH * c->scale;
    c->cell_h = FONT_HEIGHT * c->scale;
    c->cols = fb->width / c->cell_w;
    c->rows = fb->height / c->cell_h;
    c->cx = c->cy = 0;
    c->fg = pack(fb, 0xd0, 0xd0, 0xd0);
    c->bg = pack(fb, 0x00, 0x00, 0x00);
    c->sink.name = "fbcon";
    c->sink.write = fbcon_write;
    if (c->cols == 0 || c->rows == 0) {
        kwarn("fbcon: %ux%u is smaller than one character cell", c->width, c->height);
        return false;
    }
    return true;
}

static void fbcon_start(struct fbcon *c)
{
    memset(c->text, ' ', (size_t)c->cols * c->rows);
    /* A scroll repaints the screen. With 16-row cells a screen holds few
     * rows, so it moves a quarter of itself at once rather than the eighth
     * it moved with 8x8 cells: the same repaints per line of text. */
    c->scroll_rows = c->rows / 4 ? c->rows / 4 : 1;
    c->ready = true;
    fbcon_draw_clear_rows(c, 0, c->rows);
    damage(c, 0, c->height);
}

/*
 * The early console (docs/kernel/diagnostics/design.md, "The framebuffer
 * console"): when the machine has no serial console, kernel_main calls
 * this before its first line, so a boot that fails is visible on the
 * screen. It draws through the loader's direct map, which the
 * architecture carries into its own tables for this range (AArch64:
 * map_early_devices), with a static character buffer: there is no heap
 * yet. fbcon_init later keeps it as it is.
 */
#define FBCON_EARLY_CELLS (64u * 1024u)
static char g_early_text[FBCON_EARLY_CELLS];

void fbcon_early_init(void)
{
    const struct bootinfo_framebuffer *fb = bootinfo_framebuffer();
    const struct cosmoboot_info *info = bootinfo_get();
    if (fb == NULL || fb->phys + fb->size > info->hhdm_size || overlaps_usable_ram(fb->phys, fb->size))
        return;
    struct fbcon *c = &g_fbcon;
    if (!fbcon_setup(c, fb, (volatile uint8_t *)bootinfo_phys_to_virt(fb->phys)))
        return;
    if ((size_t)c->cols * c->rows > FBCON_EARLY_CELLS) {
        c->rows = FBCON_EARLY_CELLS / c->cols;   /* a very large mode: use the top of it */
        if (c->rows == 0)
            return;
    }
    c->text = g_early_text;
    fbcon_start(c);
    console_register(&c->sink);
    c->registered = true;
    kinfo("fbcon: no serial console: the framebuffer is the console from the first line (%ux%u cells)", c->cols,
          c->rows);
}

void fbcon_init(void)
{
    if (g_fbcon.ready)
        return;   /* fbcon_early_init has it, through a mapping that stays */
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
    if (!fbcon_setup(c, fb, (volatile uint8_t *)(va + (vaddr_t)offset))) {
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
    fbcon_start(c);
    replay_log(c);
    console_register(&c->sink);
    c->registered = true;
    kinfo("fbcon: %ux%u cells of %ux%u pixels at %p, scrolling %u rows at a time", c->cols, c->rows,
          c->cell_w, c->cell_h, (void *)(uintptr_t)c->base, c->scroll_rows);
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
    out->scale = g_fbcon.scale;
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

/*
 * A display driver's surface (fbcon.h). The new text buffer and the log's
 * newest screenful are taken first, in thread context; the switch itself
 * is one hold of the console lock, so no sink write sees half of it. The
 * boot framebuffer's console is kept for fbcon_detach and keeps its
 * mapping; a previous surface's text buffer is freed after the switch.
 */
int fbcon_attach(const struct fbcon_surface *s)
{
    if (s == NULL || s->base == NULL || s->bpp != 32 || s->width == 0 || s->height == 0 ||
        s->pitch < s->width * 4u)
        return -EINVAL;
    struct bootinfo_framebuffer fb = {
        .width = s->width, .height = s->height, .pitch = s->pitch, .bpp = s->bpp,
        .red_shift = s->red_shift, .red_bits = s->red_bits,
        .green_shift = s->green_shift, .green_bits = s->green_bits,
        .blue_shift = s->blue_shift, .blue_bits = s->blue_bits,
    };
    struct fbcon next;
    memset(&next, 0, sizeof(next));
    if (!fbcon_setup(&next, &fb, (volatile uint8_t *)s->base))
        return -EINVAL;
    next.text = kmalloc((size_t)next.cols * next.rows, 0);
    if (next.text == NULL)
        return -ENOMEM;
    char *log = kmalloc(KLOG_RING_SIZE, 0);
    size_t log_len = log ? klog_copy(log, KLOG_RING_SIZE) : 0;

    struct fbcon *c = &g_fbcon;
    arch_irq_state_t st = console_hold();
    char *old_text = c->surface != NULL ? c->text : NULL;
    if (c->surface == NULL)
        g_boot_fbcon = *c;   /* the boot framebuffer's console, or none (ready false) */
    /* The sink is on the console's list (its `next` links the sinks after
     * it): it stays as it is, whatever else is replaced. */
    struct console_sink sink = c->sink;
    struct fbcon_stats stats = c->stats;
    bool registered = c->registered;
    *c = next;
    c->sink = sink;
    c->stats = stats;
    c->registered = registered;
    c->sink.name = "fbcon";
    c->sink.write = fbcon_write;
    c->surface = s;
    fbcon_start(c);
    console_release(st);

    /* The newest screenful, as fbcon_init shows it. */
    if (log != NULL) {
        size_t start = log_len;
        unsigned lines = 0;
        while (start > 0 && lines < c->rows) {
            start--;
            if (log[start] == '\n' && ++lines == c->rows) {
                start++;
                break;
            }
        }
        st = console_hold();
        fbcon_write(&c->sink, log + start, log_len - start);
        console_release(st);
        kfree(log);
    }
    if (!registered) {
        console_register(&c->sink);
        c->registered = true;
    }
    kfree(old_text);
    kinfo("fbcon: %ux%u cells of %ux%u pixels on a %ux%u surface, scrolling %u rows at a time", c->cols, c->rows,
          c->cell_w, c->cell_h, c->width, c->height, c->scroll_rows);
    return 0;
}

void fbcon_detach(const struct fbcon_surface *s)
{
    struct fbcon *c = &g_fbcon;
    arch_irq_state_t st = console_hold();
    if (c->surface != s) {
        console_release(st);
        return;
    }
    char *old_text = c->text;
    struct console_sink sink = c->sink;   /* on the console's list: kept (fbcon_attach) */
    struct fbcon_stats stats = c->stats;
    bool registered = c->registered;
    *c = g_boot_fbcon;   /* the boot framebuffer again, or no console (ready false) */
    c->sink = sink;
    c->stats = stats;
    c->registered = registered;
    c->sink.name = "fbcon";
    c->sink.write = fbcon_write;
    if (c->ready) {
        /* The boot framebuffer was not drawn while the surface was. */
        memset(c->text, ' ', (size_t)c->cols * c->rows);
        c->cx = c->cy = 0;
        fbcon_draw_clear_rows(c, 0, c->rows);
    }
    console_release(st);
    kfree(old_text);
}

bool fbcon_take_damage(const struct fbcon_surface *s, uint32_t *y0, uint32_t *y1)
{
    struct fbcon *c = &g_fbcon;
    arch_irq_state_t st = console_hold();
    bool any = c->surface == s && c->dmg_y0 != c->dmg_y1;
    if (any) {
        *y0 = c->dmg_y0;
        *y1 = c->dmg_y1;
        c->dmg_y0 = c->dmg_y1 = 0;
    }
    console_release(st);
    return any;
}

/* Module ABI exports (docs/kernel/module/api.md): the display drivers'. */
#include <kernel/module.h>
EXPORT_SYMBOL(fbcon_attach);
EXPORT_SYMBOL(fbcon_detach);
EXPORT_SYMBOL(fbcon_take_damage);
